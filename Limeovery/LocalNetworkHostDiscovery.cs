using System;
using System.Collections.Generic;
using System.Linq;
using System.Net;
using System.Net.NetworkInformation;
using System.Net.Sockets;
using System.Threading;
using System.Threading.Tasks;

namespace Limeovery
{
    internal static class LocalNetworkHostDiscovery
    {
        private const int ProbeTimeoutMs = 300;
        private const int MaxConcurrentProbes = 32;
        private const int SmallestAllowedPrefix = 23;

        public static async Task<TailscaleDiscoveryResult> FindRunningHostsAsync()
        {
            List<IPAddress> addresses;
            try
            {
                addresses = await Task.Run(GetLocalLanTargets).ConfigureAwait(false);
            }
            catch (Exception ex)
            {
                return new TailscaleDiscoveryResult
                {
                    Devices = new List<RemoteDesktopDevice>(),
                    Error = "Не удалось определить локальную сеть: " + ex.Message
                };
            }

            using (var concurrencyLimit = new SemaphoreSlim(MaxConcurrentProbes))
            {
                Task<RemoteDesktopDevice>[] probes = addresses.Select(async address =>
                {
                    await concurrencyLimit.WaitAsync().ConfigureAwait(false);
                    try
                    {
                        string machineName = await TailscaleHostDiscovery
                            .ProbeLimeoveryHostAsync(address, ProbeTimeoutMs).ConfigureAwait(false);
                        if (machineName == null)
                        {
                            return null;
                        }

                        return new RemoteDesktopDevice
                        {
                            Name = string.IsNullOrWhiteSpace(machineName) ? address.ToString() : machineName,
                            MachineKey = machineName,
                            Network = "Локальная сеть",
                            Address = address,
                            Port = 5010
                        };
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

        private static List<IPAddress> GetLocalLanTargets()
        {
            var targets = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            foreach (NetworkInterface network in NetworkInterface.GetAllNetworkInterfaces())
            {
                if (network.OperationalStatus != OperationalStatus.Up || !IsLanInterface(network.NetworkInterfaceType))
                {
                    continue;
                }

                foreach (UnicastIPAddressInformation unicast in network.GetIPProperties().UnicastAddresses)
                {
                    IPAddress address = unicast.Address;
                    if (address.AddressFamily != AddressFamily.InterNetwork || !IsPrivateLanAddress(address))
                    {
                        continue;
                    }

                    IPAddress mask = unicast.IPv4Mask;
                    if (mask == null)
                    {
                        continue;
                    }

                    uint maskValue = ToUInt32(mask.GetAddressBytes());
                    int prefixLength = GetPrefixLength(maskValue);
                    if (prefixLength < SmallestAllowedPrefix || prefixLength > 30)
                    {
                        continue;
                    }

                    uint localValue = ToUInt32(address.GetAddressBytes());
                    uint networkValue = localValue & maskValue;
                    uint broadcastValue = networkValue | ~maskValue;
                    for (uint value = networkValue + 1; value < broadcastValue; value++)
                    {
                        if (value != localValue)
                        {
                            targets.Add(FromUInt32(value).ToString());
                        }
                    }
                }
            }

            return targets.Select(IPAddress.Parse).ToList();
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

        private static bool IsPrivateLanAddress(IPAddress address)
        {
            byte[] bytes = address.GetAddressBytes();
            return bytes[0] == 10 ||
                   (bytes[0] == 172 && bytes[1] >= 16 && bytes[1] <= 31) ||
                   (bytes[0] == 192 && bytes[1] == 168) ||
                   (bytes[0] == 169 && bytes[1] == 254);
        }

        private static int GetPrefixLength(uint mask)
        {
            int prefixLength = 0;
            bool sawZero = false;
            for (uint bit = 0x80000000; bit != 0; bit >>= 1)
            {
                if ((mask & bit) != 0)
                {
                    if (sawZero)
                    {
                        return -1;
                    }
                    prefixLength++;
                }
                else
                {
                    sawZero = true;
                }
            }
            return prefixLength;
        }

        private static uint ToUInt32(byte[] bytes)
        {
            return ((uint)bytes[0] << 24) |
                   ((uint)bytes[1] << 16) |
                   ((uint)bytes[2] << 8) |
                   bytes[3];
        }

        private static IPAddress FromUInt32(uint value)
        {
            return new IPAddress(new[]
            {
                (byte)(value >> 24),
                (byte)(value >> 16),
                (byte)(value >> 8),
                (byte)value
            });
        }
    }
}
