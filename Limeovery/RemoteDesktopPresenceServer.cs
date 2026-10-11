using System;
using System.Net;
using System.Net.NetworkInformation;
using System.Net.Sockets;
using System.Text;
using System.Threading;
using System.Threading.Tasks;

namespace Limeovery
{
    // A small, separate endpoint lets peers detect Limeovery without opening
    // the one-client H.264 listener on port 5010.
    internal sealed class RemoteDesktopPresenceServer : IDisposable
    {
        public const int Port = 5012;
        private const string Probe = "LIMEOVERY_PROBE/1";
        private readonly Func<bool> hostIsRunning;
        private TcpListener listener;
        private CancellationTokenSource cancellation;

        public string Error { get; private set; }
        public bool IsAvailable { get; private set; }

        public RemoteDesktopPresenceServer(Func<bool> isHostRunning)
        {
            hostIsRunning = isHostRunning;
        }

        public bool Start()
        {
            try
            {
                listener = new TcpListener(IPAddress.Any, Port);
                listener.Start(16);
                cancellation = new CancellationTokenSource();
                CancellationToken token = cancellation.Token;
                IsAvailable = true;
                Task.Run(() => AcceptLoopAsync(token));
                return true;
            }
            catch (Exception ex)
            {
                Error = ex.Message;
                IsAvailable = false;
                if (listener != null)
                {
                    listener.Stop();
                    listener = null;
                }
                return false;
            }
        }

        private async Task AcceptLoopAsync(CancellationToken token)
        {
            while (!token.IsCancellationRequested)
            {
                TcpClient client = null;
                try
                {
                    client = await listener.AcceptTcpClientAsync().ConfigureAwait(false);
                    TcpClient acceptedClient = client;
                    client = null;
                    Task.Run(() => HandleClientAsync(acceptedClient));
                }
                catch (ObjectDisposedException)
                {
                    break;
                }
                catch (SocketException)
                {
                    if (token.IsCancellationRequested)
                    {
                        break;
                    }
                }
                catch
                {
                    if (token.IsCancellationRequested)
                    {
                        break;
                    }
                }
                finally
                {
                    if (client != null)
                    {
                        client.Dispose();
                    }
                }
            }
        }

        private async Task HandleClientAsync(TcpClient client)
        {
            using (client)
            using (NetworkStream stream = client.GetStream())
            {
                try
                {
                    var endpoint = client.Client.RemoteEndPoint as IPEndPoint;
                    if (endpoint == null || !IsAllowedPeerAddress(endpoint.Address))
                    {
                        return;
                    }

                    using (var reader = new System.IO.StreamReader(stream, Encoding.ASCII, false, 256, true))
                    {
                        Task<string> requestTask = reader.ReadLineAsync();
                        if (await Task.WhenAny(requestTask, Task.Delay(1200)).ConfigureAwait(false) != requestTask)
                        {
                            return;
                        }

                        string value = await requestTask.ConfigureAwait(false);
                        if (!string.Equals(value, Probe, StringComparison.Ordinal))
                        {
                            return;
                        }

                        string machineName = Uri.EscapeDataString(Environment.MachineName);
                        string response = "LIMEOVERY/1|" + (hostIsRunning() ? "1" : "0") + "|" + machineName + "\n";
                        byte[] bytes = Encoding.ASCII.GetBytes(response);
                        await stream.WriteAsync(bytes, 0, bytes.Length).ConfigureAwait(false);
                    }
                }
                catch
                {
                    // A short probe from a peer may disconnect at any point.
                }
            }
        }

        private static bool IsAllowedPeerAddress(IPAddress address)
        {
            if (address == null || address.AddressFamily != AddressFamily.InterNetwork)
            {
                return false;
            }

            byte[] bytes = address.GetAddressBytes();
            if (bytes[0] == 100 && bytes[1] >= 64 && bytes[1] <= 127)
            {
                return true;
            }

            if (!IsPrivateLanAddress(bytes))
            {
                return false;
            }

            foreach (NetworkInterface network in NetworkInterface.GetAllNetworkInterfaces())
            {
                if (network.OperationalStatus != OperationalStatus.Up || !IsLanInterface(network.NetworkInterfaceType))
                {
                    continue;
                }

                foreach (UnicastIPAddressInformation unicast in network.GetIPProperties().UnicastAddresses)
                {
                    if (unicast.Address.AddressFamily != AddressFamily.InterNetwork || unicast.IPv4Mask == null)
                    {
                        continue;
                    }

                    byte[] local = unicast.Address.GetAddressBytes();
                    byte[] mask = unicast.IPv4Mask.GetAddressBytes();
                    bool sameSubnet = true;
                    for (int i = 0; i < 4; i++)
                    {
                        if ((bytes[i] & mask[i]) != (local[i] & mask[i]))
                        {
                            sameSubnet = false;
                            break;
                        }
                    }

                    if (sameSubnet)
                    {
                        return true;
                    }
                }
            }

            return false;
        }

        private static bool IsPrivateLanAddress(byte[] bytes)
        {
            return bytes[0] == 10 ||
                   (bytes[0] == 172 && bytes[1] >= 16 && bytes[1] <= 31) ||
                   (bytes[0] == 192 && bytes[1] == 168) ||
                   (bytes[0] == 169 && bytes[1] == 254);
        }

        private static bool IsLanInterface(NetworkInterfaceType type)
        {
            return type == NetworkInterfaceType.Ethernet ||
                   type == NetworkInterfaceType.GigabitEthernet ||
                   type == NetworkInterfaceType.FastEthernetT ||
                   type == NetworkInterfaceType.FastEthernetFx ||
                   type == NetworkInterfaceType.Ethernet3Megabit ||
                   type == NetworkInterfaceType.Wireless80211;
        }

        public void Dispose()
        {
            if (cancellation != null)
            {
                cancellation.Cancel();
                cancellation.Dispose();
                cancellation = null;
            }

            if (listener != null)
            {
                listener.Stop();
                listener = null;
            }

            IsAvailable = false;
        }
    }
}
