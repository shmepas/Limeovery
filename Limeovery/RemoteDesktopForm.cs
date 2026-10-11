using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Linq;
using System.Threading.Tasks;
using System.Windows.Forms;

namespace Limeovery
{
    internal sealed class RemoteDesktopForm : Form
    {
        private const int VideoPort = 5010;
        private readonly RemoteDesktopHostController hostController;
        private readonly string presenceServerError;
        private readonly ListView deviceList;
        private readonly Button connectButton;
        private readonly CheckBox hostEnabledBox;
        private readonly Label hostStatusLabel;
        private readonly Label discoveryStatusLabel;
        private readonly TextBox devicePinBox;
        private readonly System.Windows.Forms.Timer refreshTimer;
        private bool refreshing;
        private bool updatingHostToggle;

        public RemoteDesktopForm(RemoteDesktopHostController controller, RemoteDesktopPresenceServer presenceServer)
        {
            hostController = controller;
            presenceServerError = presenceServer == null ? "Служба обнаружения не создана." : presenceServer.Error;
            Text = "Устройства Limeovery";
            ClientSize = new Size(700, 590);
            StartPosition = FormStartPosition.CenterParent;
            FormBorderStyle = FormBorderStyle.FixedDialog;
            MaximizeBox = false;
            MinimizeBox = false;
            BackColor = Color.FromArgb(27, 38, 59);
            ForeColor = Color.FromArgb(220, 220, 220);
            Font = new Font("Segoe UI", 9F);

            Controls.Add(new Label
            {
                Text = "Устройства с запущенным Host появятся здесь. Поиск работает в локальной сети и через приватную сеть Tailscale.",
                Location = new Point(18, 14),
                Size = new Size(664, 42),
                ForeColor = ForeColor
            });

            deviceList = new ListView
            {
                Location = new Point(18, 64),
                Size = new Size(664, 285),
                View = View.Details,
                FullRowSelect = true,
                GridLines = true,
                MultiSelect = false,
                HideSelection = false,
                BackColor = Color.FromArgb(237, 241, 245),
                ForeColor = Color.FromArgb(25, 35, 45)
            };
            deviceList.Columns.Add("Устройство", 220);
            deviceList.Columns.Add("IPv4", 145);
            deviceList.Columns.Add("Сеть", 145);
            deviceList.Columns.Add("Состояние", 120);
            deviceList.SelectedIndexChanged += (sender, args) => UpdateConnectButton();
            deviceList.DoubleClick += (sender, args) => ConnectSelectedDevice();
            Controls.Add(deviceList);

            var refreshButton = CreateButton("Обновить список", 18, 362, 155);
            refreshButton.Click += async (sender, args) => await RefreshDeviceListAsync();
            Controls.Add(refreshButton);

            connectButton = CreateButton("Подключиться", 184, 362, 155);
            connectButton.Enabled = false;
            connectButton.Click += (sender, args) => ConnectSelectedDevice();
            Controls.Add(connectButton);

            hostEnabledBox = new CheckBox
            {
                Text = "Host включён (запускать вместе с Limeovery)",
                Location = new Point(18, 410),
                Size = new Size(664, 25),
                ForeColor = ForeColor,
                Checked = hostController.IsRunning
            };
            hostEnabledBox.CheckedChanged += HostEnabledChanged;
            Controls.Add(hostEnabledBox);

            hostStatusLabel = new Label
            {
                Location = new Point(18, 440),
                Size = new Size(664, 24),
                ForeColor = Color.FromArgb(180, 220, 180)
            };
            Controls.Add(hostStatusLabel);

            Controls.Add(new Label
            {
                Text = "Постоянный PIN:",
                Location = new Point(18, 470),
                Size = new Size(108, 26),
                ForeColor = ForeColor,
                TextAlign = ContentAlignment.MiddleLeft
            });
            devicePinBox = new TextBox
            {
                Location = new Point(127, 470),
                Size = new Size(190, 25),
                ReadOnly = true,
                ShortcutsEnabled = false,
                Font = new Font("Consolas", 11F, FontStyle.Bold),
                Text = RemoteDesktopHostController.GetOrCreateDevicePin()
            };
            Controls.Add(devicePinBox);

            var copyPinButton = CreateButton("Копировать PIN", 328, 466, 142);
            copyPinButton.Click += (sender, args) =>
            {
                try
                {
                    Clipboard.SetText(devicePinBox.Text);
                    discoveryStatusLabel.Text = "PIN скопирован. Передавайте его только доверенному человеку.";
                }
                catch (Exception ex)
                {
                    MessageBox.Show(this, "Не удалось скопировать PIN:\n" + ex.Message,
                        "Удалённый экран", MessageBoxButtons.OK, MessageBoxIcon.Error);
                }
            };
            Controls.Add(copyPinButton);

            var regeneratePinButton = CreateButton("Сменить PIN", 481, 466, 155);
            regeneratePinButton.Click += RegenerateDevicePin;
            Controls.Add(regeneratePinButton);

            Controls.Add(new Label
            {
                Text = "PIN хранится в профиле Windows. Новый PIN отзовёт старый и перезапустит Host.",
                Location = new Point(18, 498),
                Size = new Size(664, 30),
                ForeColor = Color.FromArgb(190, 200, 215)
            });

            discoveryStatusLabel = new Label
            {
                Text = "Проверяю локальную сеть и Tailscale…",
                Location = new Point(18, 535),
                Size = new Size(664, 38),
                ForeColor = Color.FromArgb(190, 200, 215)
            };
            Controls.Add(discoveryStatusLabel);

            refreshTimer = new System.Windows.Forms.Timer { Interval = 5000 };
            refreshTimer.Tick += async (sender, args) =>
            {
                UpdateHostStatus();
                await RefreshDeviceListAsync();
            };
            Shown += async (sender, args) =>
            {
                UpdateHostStatus();
                await RefreshDeviceListAsync();
                refreshTimer.Start();
            };
            FormClosed += (sender, args) =>
            {
                refreshTimer.Stop();
                refreshTimer.Dispose();
            };
        }

        private static Button CreateButton(string text, int x, int y, int width)
        {
            var button = new Button
            {
                Text = text,
                Location = new Point(x, y),
                Size = new Size(width, 34),
                FlatStyle = FlatStyle.Flat,
                BackColor = Color.FromArgb(60, 140, 60),
                ForeColor = Color.White,
                Cursor = Cursors.Hand
            };
            button.FlatAppearance.BorderSize = 0;
            return button;
        }

        private async Task RefreshDeviceListAsync()
        {
            if (refreshing || IsDisposed)
            {
                return;
            }

            refreshing = true;
            discoveryStatusLabel.Text = "Ищу Limeovery в локальной сети и Tailscale…";
            try
            {
                Task<TailscaleDiscoveryResult> localTask = LocalNetworkHostDiscovery.FindRunningHostsAsync();
                Task<TailscaleDiscoveryResult> tailscaleTask = TailscaleHostDiscovery.FindRunningHostsAsync();
                await Task.WhenAll(localTask, tailscaleTask);
                if (IsDisposed)
                {
                    return;
                }

                TailscaleDiscoveryResult localResult = await localTask;
                TailscaleDiscoveryResult tailscaleResult = await tailscaleTask;
                List<RemoteDesktopDevice> devices = MergeDevices(localResult.Devices, tailscaleResult.Devices);

                deviceList.BeginUpdate();
                try
                {
                    deviceList.Items.Clear();
                    foreach (RemoteDesktopDevice device in devices)
                    {
                        var item = new ListViewItem(device.Name);
                        item.SubItems.Add(device.Address.ToString());
                        item.SubItems.Add(device.Network);
                        item.SubItems.Add("Host активен");
                        item.Tag = device;
                        deviceList.Items.Add(item);
                    }
                }
                finally
                {
                    deviceList.EndUpdate();
                }
                UpdateConnectButton();

                if (!string.IsNullOrEmpty(presenceServerError))
                {
                    discoveryStatusLabel.Text = "Не запущена служба обнаружения (TCP 5012): " + presenceServerError;
                }
                else if (devices.Count > 0)
                {
                    int localCount = devices.Count(device => device.Network == "Локальная сеть");
                    int tailscaleCount = devices.Count - localCount;
                    discoveryStatusLabel.Text = "Найдено устройств: " + devices.Count +
                        " (локальная сеть: " + localCount + ", Tailscale: " + tailscaleCount + "). Список обновляется автоматически.";
                }
                else if (!string.IsNullOrEmpty(localResult.Error) || !string.IsNullOrEmpty(tailscaleResult.Error))
                {
                    string localError = string.IsNullOrEmpty(localResult.Error) ? "" : localResult.Error;
                    string tailscaleError = string.IsNullOrEmpty(tailscaleResult.Error) ? "" : tailscaleResult.Error;
                    discoveryStatusLabel.Text = "Устройства не найдены. " + localError +
                        (localError.Length > 0 && tailscaleError.Length > 0 ? " " : "") + tailscaleError;
                }
                else
                {
                    discoveryStatusLabel.Text = "Устройства не найдены. Проверь, что Host запущен и брандмауэр Windows разрешает TCP-порты 5010 и 5012.";
                }
            }
            catch (Exception ex)
            {
                if (!IsDisposed)
                {
                    discoveryStatusLabel.Text = "Не удалось обновить список: " + ex.Message;
                }
            }
            finally
            {
                refreshing = false;
            }
        }

        private static List<RemoteDesktopDevice> MergeDevices(
            IList<RemoteDesktopDevice> localDevices,
            IList<RemoteDesktopDevice> tailscaleDevices)
        {
            var merged = new Dictionary<string, RemoteDesktopDevice>(StringComparer.OrdinalIgnoreCase);
            IEnumerable<RemoteDesktopDevice> allDevices =
                (localDevices ?? new RemoteDesktopDevice[0]).Concat(tailscaleDevices ?? new RemoteDesktopDevice[0]);

            foreach (RemoteDesktopDevice device in allDevices)
            {
                string key = !string.IsNullOrWhiteSpace(device.MachineKey)
                    ? "name:" + device.MachineKey.Trim()
                    : "ip:" + device.Address;
                RemoteDesktopDevice existing;
                if (!merged.TryGetValue(key, out existing) ||
                    (existing.Network != "Локальная сеть" && device.Network == "Локальная сеть"))
                {
                    merged[key] = device;
                }
            }

            return merged.Values.OrderBy(device => device.Name).ToList();
        }

        private void HostEnabledChanged(object sender, EventArgs e)
        {
            if (updatingHostToggle)
            {
                return;
            }

            bool enabled = hostEnabledBox.Checked;
            try
            {
                RemoteDesktopHostController.SaveAutoStartEnabled(enabled);
                if (enabled)
                {
                    string error;
                    if (!hostController.Start(out error))
                    {
                        RemoteDesktopHostController.SaveAutoStartEnabled(false);
                        SetHostToggle(false);
                        MessageBox.Show(this, error, "Удалённый экран",
                            MessageBoxButtons.OK, MessageBoxIcon.Error);
                        return;
                    }
                }
                else
                {
                    hostController.Stop();
                }

                UpdateHostStatus();
            }
            catch (Exception ex)
            {
                SetHostToggle(hostController.IsRunning);
                MessageBox.Show(this, "Не удалось изменить настройку Host:\n" + ex.Message,
                    "Удалённый экран", MessageBoxButtons.OK, MessageBoxIcon.Error);
            }
        }

        private void SetHostToggle(bool value)
        {
            updatingHostToggle = true;
            hostEnabledBox.Checked = value;
            updatingHostToggle = false;
        }

        private void UpdateHostStatus()
        {
            bool running = hostController.IsRunning;
            if (hostEnabledBox.Checked != running)
            {
                SetHostToggle(running);
            }

            hostStatusLabel.Text = running
                ? "Host активен на TCP-порту " + VideoPort + "; закрой Limeovery, чтобы остановить его."
                : "Host выключен.";
        }

        private void UpdateConnectButton()
        {
            connectButton.Enabled = deviceList.SelectedItems.Count == 1;
        }

        private void ConnectSelectedDevice()
        {
            if (deviceList.SelectedItems.Count != 1)
            {
                return;
            }

            var device = deviceList.SelectedItems[0].Tag as RemoteDesktopDevice;
            if (device == null)
            {
                return;
            }

            string pin = PromptForDevicePin(this);
            if (pin == null)
            {
                return;
            }

            string viewerPath = Path.Combine(
                AppDomain.CurrentDomain.BaseDirectory,
                "Tools",
                "RemoteDesktop",
                "Viewer.exe");
            if (!File.Exists(viewerPath))
            {
                MessageBox.Show(this, "Не найден Viewer.exe:\n" + viewerPath,
                    "Удалённый экран", MessageBoxButtons.OK, MessageBoxIcon.Error);
                return;
            }

            try
            {
                Process viewer = Process.Start(new ProcessStartInfo
                {
                    FileName = viewerPath,
                    Arguments = "--address " + device.Address + " --port " + device.Port + " --pin-stdin",
                    WorkingDirectory = Path.GetDirectoryName(viewerPath),
                    UseShellExecute = false,
                    RedirectStandardInput = true,
                    CreateNoWindow = true,
                    WindowStyle = ProcessWindowStyle.Hidden
                });
                if (viewer == null)
                {
                    MessageBox.Show(this, "Windows не запустила Viewer.exe.",
                        "Удалённый экран", MessageBoxButtons.OK, MessageBoxIcon.Error);
                    return;
                }
                viewer.StandardInput.WriteLine(pin);
                viewer.StandardInput.Flush();
                viewer.StandardInput.Close();
                discoveryStatusLabel.Text = "Viewer подключается к " + device.Name + " (" + device.Address + ").";
            }
            catch (Exception ex)
            {
                MessageBox.Show(this, "Не удалось запустить Viewer:\n" + ex.Message,
                    "Удалённый экран", MessageBoxButtons.OK, MessageBoxIcon.Error);
            }
        }

        private void RegenerateDevicePin(object sender, EventArgs e)
        {
            DialogResult answer = MessageBox.Show(
                this,
                "Старый PIN перестанет работать. Host будет перезапущен с новым PIN. Продолжить?",
                "Сменить PIN устройства",
                MessageBoxButtons.YesNo,
                MessageBoxIcon.Warning);
            if (answer != DialogResult.Yes)
            {
                return;
            }

            bool restartHost = hostController.IsRunning;
            try
            {
                hostController.Stop();
                devicePinBox.Text = RemoteDesktopHostController.GenerateAndSaveDevicePin();
                if (restartHost)
                {
                    string error;
                    if (!hostController.Start(out error))
                    {
                        UpdateHostStatus();
                        MessageBox.Show(this,
                            "PIN сменён, но Host не удалось перезапустить:\n" + error,
                            "Удалённый экран", MessageBoxButtons.OK, MessageBoxIcon.Error);
                        return;
                    }
                }
                UpdateHostStatus();
                discoveryStatusLabel.Text = "PIN сменён. Подключения со старым PIN больше не принимаются.";
            }
            catch (Exception ex)
            {
                MessageBox.Show(this, "Не удалось сменить PIN:\n" + ex.Message,
                    "Удалённый экран", MessageBoxButtons.OK, MessageBoxIcon.Error);
            }
        }

        private static string PromptForDevicePin(IWin32Window owner)
        {
            using (var dialog = new Form())
            using (var label = new Label())
            using (var pinBox = new TextBox())
            using (var okButton = new Button())
            using (var cancelButton = new Button())
            {
                dialog.Text = "PIN устройства";
                dialog.ClientSize = new Size(360, 145);
                dialog.StartPosition = FormStartPosition.CenterParent;
                dialog.FormBorderStyle = FormBorderStyle.FixedDialog;
                dialog.MaximizeBox = false;
                dialog.MinimizeBox = false;
                dialog.ShowInTaskbar = false;

                label.Text = "Введите 12-значный постоянный PIN, показанный на Host.";
                label.Location = new Point(16, 14);
                label.Size = new Size(328, 40);
                pinBox.Location = new Point(18, 58);
                pinBox.Size = new Size(324, 25);
                pinBox.MaxLength = 12;
                pinBox.PasswordChar = '●';
                pinBox.Font = new Font("Consolas", 12F, FontStyle.Bold);
                okButton.Text = "Подключиться";
                okButton.Location = new Point(122, 101);
                okButton.Size = new Size(112, 30);
                cancelButton.Text = "Отмена";
                cancelButton.Location = new Point(244, 101);
                cancelButton.Size = new Size(100, 30);
                cancelButton.DialogResult = DialogResult.Cancel;

                okButton.Click += (sender, args) =>
                {
                    if (pinBox.Text.Length != 12 || pinBox.Text.Any(character => character < '0' || character > '9'))
                    {
                        MessageBox.Show(dialog, "PIN должен состоять ровно из 12 цифр.",
                            "PIN устройства", MessageBoxButtons.OK, MessageBoxIcon.Information);
                        pinBox.Focus();
                        return;
                    }
                    dialog.DialogResult = DialogResult.OK;
                    dialog.Close();
                };

                dialog.Controls.Add(label);
                dialog.Controls.Add(pinBox);
                dialog.Controls.Add(okButton);
                dialog.Controls.Add(cancelButton);
                dialog.AcceptButton = okButton;
                dialog.CancelButton = cancelButton;
                return dialog.ShowDialog(owner) == DialogResult.OK ? pinBox.Text : null;
            }
        }
    }
}
