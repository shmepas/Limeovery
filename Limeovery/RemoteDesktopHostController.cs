using System;
using System.Diagnostics;
using System.IO;
using System.Security.Cryptography;
using System.Text;
using Microsoft.Win32;

namespace Limeovery
{
    internal sealed class RemoteDesktopHostController : IDisposable
    {
        private const int VideoPort = 5010;
        private const string RegistryPath = @"Software\Limeovery";
        private const string ProtectedDevicePinValue = "ProtectedDevicePin";
        private Process hostProcess;
        private bool ownsProcess;

        public static bool AutoStartEnabled
        {
            get
            {
                try
                {
                    using (RegistryKey key = Registry.CurrentUser.OpenSubKey(RegistryPath, false))
                    {
                        // First launch starts Host by default. The user can turn this off in Remote Desktop settings.
                        return key == null || Convert.ToInt32(key.GetValue("AutoStartHost", 1)) == 1;
                    }
                }
                catch
                {
                    return false;
                }
            }
        }

        public bool IsRunning
        {
            get
            {
                if (hostProcess == null)
                {
                    return false;
                }
                try
                {
                    return !hostProcess.HasExited;
                }
                catch
                {
                    return false;
                }
            }
        }

        public static void SaveAutoStartEnabled(bool enabled)
        {
            using (RegistryKey key = Registry.CurrentUser.CreateSubKey(RegistryPath))
            {
                if (key == null)
                {
                    throw new InvalidOperationException("Не удалось создать раздел настроек пользователя.");
                }
                key.SetValue("AutoStartHost", enabled ? 1 : 0, RegistryValueKind.DWord);
            }
        }

        public static string GetOrCreateDevicePin()
        {
            try
            {
                using (RegistryKey key = Registry.CurrentUser.OpenSubKey(RegistryPath, false))
                {
                    byte[] protectedPin = key == null ? null : key.GetValue(ProtectedDevicePinValue) as byte[];
                    if (protectedPin != null)
                    {
                        byte[] plainPin = ProtectedData.Unprotect(
                            protectedPin,
                            null,
                            DataProtectionScope.CurrentUser);
                        try
                        {
                            string storedPin = Encoding.UTF8.GetString(plainPin);
                            if (IsValidDevicePin(storedPin))
                            {
                                return storedPin;
                            }
                        }
                        finally
                        {
                            Array.Clear(plainPin, 0, plainPin.Length);
                        }
                    }
                }
            }
            catch
            {
                // A missing or unreadable value is replaced with a fresh device secret.
            }

            string pin = GenerateDevicePin();
            SaveDevicePin(pin);
            return pin;
        }

        public static string GenerateAndSaveDevicePin()
        {
            string pin = GenerateDevicePin();
            SaveDevicePin(pin);
            return pin;
        }

        private static void SaveDevicePin(string pin)
        {
            if (!IsValidDevicePin(pin))
            {
                throw new ArgumentException("PIN устройства должен содержать 12 цифр.", "pin");
            }

            byte[] plainPin = Encoding.UTF8.GetBytes(pin);
            byte[] protectedPin = ProtectedData.Protect(
                plainPin,
                null,
                DataProtectionScope.CurrentUser);
            try
            {
                using (RegistryKey key = Registry.CurrentUser.CreateSubKey(RegistryPath))
                {
                    if (key == null)
                    {
                        throw new InvalidOperationException("Не удалось сохранить PIN устройства.");
                    }
                    key.SetValue(ProtectedDevicePinValue, protectedPin, RegistryValueKind.Binary);
                }
            }
            finally
            {
                Array.Clear(plainPin, 0, plainPin.Length);
                Array.Clear(protectedPin, 0, protectedPin.Length);
            }
        }

        private static string GenerateDevicePin()
        {
            var pin = new StringBuilder(12);
            using (var random = new RNGCryptoServiceProvider())
            {
                byte[] bytes = new byte[1];
                while (pin.Length < 12)
                {
                    random.GetBytes(bytes);
                    if (bytes[0] < 250)
                    {
                        pin.Append((char)('0' + bytes[0] % 10));
                    }
                }
                Array.Clear(bytes, 0, bytes.Length);
            }
            return pin.ToString();
        }

        private static bool IsValidDevicePin(string pin)
        {
            if (string.IsNullOrEmpty(pin) || pin.Length != 12)
            {
                return false;
            }
            for (int index = 0; index < pin.Length; index++)
            {
                if (pin[index] < '0' || pin[index] > '9')
                {
                    return false;
                }
            }
            return true;
        }

        public bool Start(out string error)
        {
            error = null;
            if (IsRunning)
            {
                return true;
            }

            string executablePath = Path.Combine(
                AppDomain.CurrentDomain.BaseDirectory,
                "Tools",
                "RemoteDesktop",
                "HostSilent.exe");
            if (!File.Exists(executablePath))
            {
                error = "Не найден HostSilent.exe:\n" + executablePath;
                return false;
            }

            try
            {
                string devicePin = GetOrCreateDevicePin();
                if (hostProcess != null)
                {
                    hostProcess.Dispose();
                    hostProcess = null;
                }
                hostProcess = Process.Start(new ProcessStartInfo
                {
                    FileName = executablePath,
                    Arguments = "--port " + VideoPort + " --pin-stdin",
                    WorkingDirectory = Path.GetDirectoryName(executablePath),
                    UseShellExecute = false,
                    RedirectStandardInput = true,
                    CreateNoWindow = true,
                    WindowStyle = ProcessWindowStyle.Hidden
                });
                ownsProcess = hostProcess != null;
                if (hostProcess == null)
                {
                    error = "Windows не запустила HostSilent.exe.";
                    return false;
                }
                hostProcess.StandardInput.WriteLine(devicePin);
                hostProcess.StandardInput.Flush();
                hostProcess.StandardInput.Close();
                return true;
            }
            catch (Exception ex)
            {
                Stop();
                error = "Не удалось запустить HostSilent.exe:\n" + ex.Message;
                return false;
            }
        }

        public void Stop()
        {
            if (hostProcess == null)
            {
                return;
            }

            try
            {
                if (ownsProcess && !hostProcess.HasExited)
                {
                    // Host is a console process and may have no main window to receive CloseMainWindow.
                    // Stop it immediately when Limeovery closes.
                    hostProcess.Kill();
                    hostProcess.WaitForExit(1000);
                }
            }
            catch
            {
                // Shutdown should continue even if Host already exited.
            }
            finally
            {
                hostProcess.Dispose();
                hostProcess = null;
                ownsProcess = false;
            }
        }

        public void Dispose()
        {
            Stop();
        }
    }
}
