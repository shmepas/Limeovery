using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using System.Web.Script.Serialization;

namespace Limeovery
{
    internal sealed class RemoteDesktopDevice
    {
        public string Name { get; set; }
        public string MachineKey { get; set; }
        public string Network { get; set; }
        public IPAddress Address { get; set; }
        public int Port { get; set; }
    }

    internal sealed class TailscaleDiscoveryResult
    {
        public IList<RemoteDesktopDevice> Devices { get; set; }
        public string Error { get; set; }
    }

    internal static class TailscaleHostDiscovery
    {
        private const int VideoPort = 5010;
        private const int ProbeTimeoutMs = 1500;
        private const int MaxConcurrentProbes = 16;

        private sealed class PeerReadResult
        {
            public List<RemoteDesktopDevice> Devices { get; set; }
            public string Error { get; set; }
        }

        public static async Task<TailscaleDiscoveryResult> FindRunningHostsAsync()
        {
            PeerReadResult peerRead;
            try
            {
                peerRead = await Task.Run(() =>
                {
                    string readError;
                    List<RemoteDesktopDevice> devices = ReadOnlinePeers(out readError);
                    return new PeerReadResult { Devices = devices, Error = readError };
                }).ConfigureAwait(false);
            }
            catch (Exception ex)
            {
                return new TailscaleDiscoveryResult { Devices = new List<RemoteDesktopDevice>(), Error = ex.Message };
            }

            if (!string.IsNullOrEmpty(peerRead.Error))
            {
                return new TailscaleDiscoveryResult { Devices = new List<RemoteDesktopDevice>(), Error = peerRead.Error };
            }

            using (var concurrencyLimit = new SemaphoreSlim(MaxConcurrentProbes))
            {
                Task<RemoteDesktopDevice>[] probes = peerRead.Devices.Select(async peer =>
                {
                    await concurrencyLimit.WaitAsync().ConfigureAwait(false);
                    try
                    {
                        string machineName = await ProbeLimeoveryHostAsync(peer.Address, ProbeTimeoutMs).ConfigureAwait(false);
                        if (machineName == null)
                        {
                            return null;
                        }

                        if (!string.IsNullOrWhiteSpace(machineName))
                        {
                            peer.Name = machineName;
                            peer.MachineKey = machineName;
                        }
                        else
                        {
                            peer.MachineKey = peer.Name;
                        }
                        peer.Network = "Tailscale";
                        return peer;
                    }
                    finally
                    {
                        concurrencyLimit.Release();
                    }
                }).ToArray();

                RemoteDesktopDevice[] results = await Task.WhenAll(probes).ConfigureAwait(false);
                return new TailscaleDiscoveryResult
                {
                    Devices = results.Where(device => device != null).OrderBy(device => device.Name).ToList(),
                    Error = null
                };
            }
        }

        private static List<RemoteDesktopDevice> ReadOnlinePeers(out string error)
        {
            error = null;
            string executable = FindTailscaleExecutable();
            if (executable == null)
            {
                error = "Tailscale не найден. Установи его и войди в одну приватную сеть на каждом компьютере.";
                return new List<RemoteDesktopDevice>();
            }

            string json;
            string standardError;
            using (var process = new Process())
            {
                process.StartInfo = new ProcessStartInfo
                {
                    FileName = executable,
                    Arguments = "status --json",
                    UseShellExecute = false,
                    CreateNoWindow = true,
                    RedirectStandardOutput = true,
                    RedirectStandardError = true,
                    StandardOutputEncoding = Encoding.UTF8,
                    StandardErrorEncoding = Encoding.UTF8
                };

                if (!process.Start())
                {
                    error = "Не удалось запустить Tailscale CLI.";
                    return new List<RemoteDesktopDevice>();
                }

                Task<string> outputTask = process.StandardOutput.ReadToEndAsync();
                Task<string> errorTask = process.StandardError.ReadToEndAsync();
                if (!process.WaitForExit(5000))
                {
                    try { process.Kill(); } catch { }
                    error = "Tailscale не ответил за 5 секунд.";
                    return new List<RemoteDesktopDevice>();
                }

                json = outputTask.GetAwaiter().GetResult();
                standardError = errorTask.GetAwaiter().GetResult();
                if (process.ExitCode != 0)
                {
                    error = string.IsNullOrWhiteSpace(standardError)
                        ? "Tailscale не подключён к сети. Открой Tailscale и войди в семейную сеть."
                        : standardError.Trim();
                    return new List<RemoteDesktopDevice>();
                }
            }

            var serializer = new JavaScriptSerializer { MaxJsonLength = 1024 * 1024 };
            var root = serializer.Deserialize<Dictionary<string, object>>(json);
            object peerValue;
            var peerMap = root != null && root.TryGetValue("Peer", out peerValue)
                ? peerValue as Dictionary<string, object>
                : null;
            if (peerMap == null)
            {
                error = "Tailscale не вернул список устройств. Проверь, что клиент вошёл в сеть.";
                return new List<RemoteDesktopDevice>();
            }

            var selfAddresses = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            object selfValue;
            var self = root.TryGetValue("Self", out selfValue) ? selfValue as Dictionary<string, object> : null;
            AddAddresses(self, selfAddresses);

            var devices = new List<RemoteDesktopDevice>();
            foreach (KeyValuePair<string, object> entry in peerMap)
            {
                var peer = entry.Value as Dictionary<string, object>;
                if (peer == null || IsExplicitlyOffline(peer))
                {
                    continue;
                }

                string name = ReadString(peer, "HostName");
                if (string.IsNullOrWhiteSpace(name))
                {
                    name = ReadString(peer, "DNSName");
                }

                foreach (IPAddress address in ReadIPv4Addresses(peer))
                {
                    if (selfAddresses.Contains(address.ToString()))
                    {
                        continue;
                    }

                    devices.Add(new RemoteDesktopDevice
                    {
                        Name = string.IsNullOrWhiteSpace(name) ? address.ToString() : name.TrimEnd('.'),
                        Address = address,
                        Port = VideoPort
                    });
                    break;
                }
            }

            return devices;
        }

        private static bool IsExplicitlyOffline(Dictionary<string, object> peer)
        {
            object online;
            return peer.TryGetValue("Online", out online) && online is bool && !(bool)online;
        }

        private static string ReadString(Dictionary<string, object> values, string key)
        {
            object value;
            return values.TryGetValue(key, out value) ? value as string : null;
        }

        private static IEnumerable<IPAddress> ReadIPv4Addresses(Dictionary<string, object> values)
        {
            object addressesValue;
            object[] addresses = values.TryGetValue("TailscaleIPs", out addressesValue)
                ? addressesValue as object[]
                : null;
            if (addresses == null)
            {
                yield break;
            }

            foreach (object item in addresses)
            {
                IPAddress address;
                if (item is string && IPAddress.TryParse((string)item, out address) &&
                    address.AddressFamily == AddressFamily.InterNetwork)
                {
                    yield return address;
                }
            }
        }

        private static void AddAddresses(Dictionary<string, object> values, ISet<string> output)
        {
            if (values == null)
            {
                return;
            }
            foreach (IPAddress address in ReadIPv4Addresses(values))
            {
                output.Add(address.ToString());
            }
        }

        internal static async Task<string> ProbeLimeoveryHostAsync(IPAddress address, int timeoutMs)
        {
            using (var client = new TcpClient(AddressFamily.InterNetwork))
            {
                try
                {
                    Task connect = client.ConnectAsync(address, RemoteDesktopPresenceServer.Port);
                    Task timeout = Task.Delay(timeoutMs);
                    if (await Task.WhenAny(connect, timeout).ConfigureAwait(false) != connect)
                    {
                        client.Close();
                        return null;
                    }
                    await connect.ConfigureAwait(false);

                    using (NetworkStream stream = client.GetStream())
                    {
                        using (var writer = new StreamWriter(stream, Encoding.ASCII, 256, true))
                        using (var reader = new StreamReader(stream, Encoding.ASCII, false, 256, true))
                        {
                            writer.AutoFlush = true;
                            await writer.WriteLineAsync("LIMEOVERY_PROBE/1").ConfigureAwait(false);
                            Task<string> read = reader.ReadLineAsync();
                            if (await Task.WhenAny(read, Task.Delay(timeoutMs)).ConfigureAwait(false) != read)
                            {
                                return null;
                            }

                            string value = await read.ConfigureAwait(false);
                            if (string.IsNullOrEmpty(value))
                            {
                                return null;
                            }

                            string[] fields = value.Split('|');
                            if (fields.Length < 2 || !string.Equals(fields[0], "LIMEOVERY/1", StringComparison.Ordinal) ||
                                !string.Equals(fields[1], "1", StringComparison.Ordinal))
                            {
                                return null;
                            }

                            return fields.Length >= 3 ? Uri.UnescapeDataString(fields[2]) : string.Empty;
                        }
                    }
                }
                catch
                {
                    return null;
                }
            }
        }

        private static string FindTailscaleExecutable()
        {
            string[] candidates =
            {
                Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles), "Tailscale", "tailscale.exe"),
                Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFilesX86), "Tailscale", "tailscale.exe")
            };
            foreach (string candidate in candidates)
            {
                if (File.Exists(candidate))
                {
                    return candidate;
                }
            }

            string path = Environment.GetEnvironmentVariable("PATH") ?? string.Empty;
            foreach (string directory in path.Split(Path.PathSeparator))
            {
                try
                {
                    string candidate = Path.Combine(directory.Trim(), "tailscale.exe");
                    if (File.Exists(candidate))
                    {
                        return candidate;
                    }
                }
                catch
                {
                    // Ignore malformed PATH entries.
                }
            }

            return null;
        }
    }
}
