using System;
using System.Diagnostics;
using System.IO;
using Microsoft.Win32;

namespace Limeovery
{
    internal sealed class RemoteDesktopHostController : IDisposable
    {
        private const int VideoPort = 5010;
        private const string RegistryPath = @"Software\Limeovery";
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
                if (hostProcess != null)
                {
                    hostProcess.Dispose();
                    hostProcess = null;
                }
                hostProcess = Process.Start(new ProcessStartInfo
                {
                    FileName = executablePath,
                    Arguments = "--port " + VideoPort,
                    WorkingDirectory = Path.GetDirectoryName(executablePath),
                    UseShellExecute = false,
                    CreateNoWindow = true,
                    WindowStyle = ProcessWindowStyle.Hidden
                });
                ownsProcess = hostProcess != null;
                if (hostProcess == null)
                {
                    error = "Windows не запустила HostSilent.exe.";
                    return false;
                }
                return true;
            }
            catch (Exception ex)
            {
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
