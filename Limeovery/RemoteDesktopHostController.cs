using System;
using System.Diagnostics;
using System.IO;
using System.Security.Cryptography;
using System.Text;
using System.Threading;
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
        private readonly ManualResetEventSlim hostReady = new ManualResetEventSlim(false);
        private readonly object logLock = new object();
        private readonly StringBuilder recentOutput = new StringBuilder();
        private StreamWriter logWriter;
        private string hostLogPath;
        private bool exitLogged;

        private const string ReadyMarker = "LIMEOVERY_HOST_READY";
        private const int StartupTimeoutSeconds = 25;

        public string HostLogPath
        {
            get { return hostLogPath ?? GetDefaultHostLogPath(); }
        }

        public string LastExitSummary { get; private set; }

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
                    if (!hostProcess.HasExited)
                    {
                        return true;
                    }

                    if (!exitLogged)
                    {
                        hostProcess.WaitForExit();
                        int exitCode = hostProcess.ExitCode;
                        AppendHostOutput("INFO", "Host exited with code " + exitCode + ".");
                        LastExitSummary = BuildStartupError("Host завершился (код " + exitCode + ").");
                        exitLogged = true;
                    }
                    return false;
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
                BeginLog(executablePath);
                hostReady.Reset();
                recentOutput.Clear();
                exitLogged = false;
                LastExitSummary = null;

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
                    RedirectStandardOutput = true,
                    RedirectStandardError = true,
                    StandardOutputEncoding = Encoding.UTF8,
                    StandardErrorEncoding = Encoding.UTF8,
                    CreateNoWindow = true,
                    WindowStyle = ProcessWindowStyle.Hidden
                });
                ownsProcess = hostProcess != null;
                if (hostProcess == null)
                {
                    error = "Windows не запустила HostSilent.exe.";
                    return false;
                }

                hostProcess.OutputDataReceived += HostOutputReceived;
                hostProcess.ErrorDataReceived += HostErrorReceived;
                hostProcess.BeginOutputReadLine();
                hostProcess.BeginErrorReadLine();
                hostProcess.StandardInput.WriteLine(devicePin);
                hostProcess.StandardInput.Flush();
                hostProcess.StandardInput.Close();

                DateTime deadline = DateTime.UtcNow.AddSeconds(StartupTimeoutSeconds);
                while (!hostReady.Wait(100))
                {
                    if (hostProcess.HasExited)
                    {
                        hostProcess.WaitForExit();
                        error = BuildStartupError("Host завершился во время запуска (код " +
                            hostProcess.ExitCode + ").");
                        Stop();
                        return false;
                    }
                    if (DateTime.UtcNow >= deadline)
                    {
                        error = BuildStartupError("Host не сообщил о готовности за " +
                            StartupTimeoutSeconds + " секунд.");
                        Stop();
                        return false;
                    }
                }

                return true;
            }
            catch (Exception ex)
            {
                error = BuildStartupError("Не удалось запустить HostSilent.exe:\n" + ex.Message);
                Stop();
                return false;
            }
        }

        private void BeginLog(string executablePath)
        {
            hostLogPath = GetDefaultHostLogPath();
            string directory = Path.GetDirectoryName(hostLogPath);
            Directory.CreateDirectory(directory);
            lock (logLock)
            {
                if (logWriter != null)
                {
                    logWriter.Dispose();
                }
                logWriter = new StreamWriter(hostLogPath, false, new UTF8Encoding(false));
                logWriter.AutoFlush = true;
                recentOutput.Clear();
                WriteLogLine("Starting Host: " + executablePath);
                WriteLogLine("Windows: " + Environment.OSVersion.VersionString +
                    "; Limeovery UI process architecture: " + (Environment.Is64BitProcess ? "x64" : "x86"));
            }
        }

        private static string GetDefaultHostLogPath()
        {
            return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                "Limeovery", "Logs", "Host.log");
        }

        private void HostOutputReceived(object sender, DataReceivedEventArgs args)
        {
            if (args.Data == null)
            {
                return;
            }
            AppendHostOutput("OUT", args.Data);
            if (string.Equals(args.Data.Trim(), ReadyMarker, StringComparison.Ordinal))
            {
                hostReady.Set();
            }
        }

        private void HostErrorReceived(object sender, DataReceivedEventArgs args)
        {
            if (args.Data != null)
            {
                AppendHostOutput("ERR", args.Data);
            }
        }

        private void AppendHostOutput(string stream, string line)
        {
            lock (logLock)
            {
                WriteLogLine(stream + ": " + line);
                recentOutput.AppendLine(line);
                if (recentOutput.Length > 12000)
                {
                    recentOutput.Remove(0, recentOutput.Length - 12000);
                }
            }
        }

        private void WriteLogLine(string line)
        {
            if (logWriter != null)
            {
                logWriter.WriteLine(DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss.fff") + " " + line);
            }
        }

        private string BuildStartupError(string summary)
        {
            var message = new StringBuilder(summary);
            lock (logLock)
            {
                if (recentOutput.Length > 0)
                {
                    message.AppendLine().AppendLine().AppendLine("Последние сообщения Host:")
                        .Append(recentOutput.ToString().Trim());
                }
                message.AppendLine().AppendLine("Полный журнал: ").Append(HostLogPath);
            }
            return message.ToString();
        }

        public void Stop()
        {
            try
            {
                if (hostProcess != null && ownsProcess && !hostProcess.HasExited)
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
                if (logWriter != null)
                {
                    lock (logLock)
                    {
                        logWriter.Dispose();
                        logWriter = null;
                    }
                }
                if (hostProcess != null)
                {
                    hostProcess.Dispose();
                    hostProcess = null;
                }
                ownsProcess = false;
            }
        }

        public void Dispose()
        {
            Stop();
            hostReady.Dispose();
        }
    }
}
