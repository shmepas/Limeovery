using Microsoft.Win32;
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Data;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Linq;
using System.Management;
using System.Net;
using System.Net.NetworkInformation;
using System.Net.Sockets;
using System.Reflection;
using System.Security.Principal;
using System.ServiceProcess;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using System.Windows.Forms;

namespace Limeovery
{
    public partial class Form1 : Form
    {
        public static string TargetIP = "192.168.3.19";
        public static int TargetPort = 5000;

        public Form1() { InitializeComponent(); StartServer(); }

        private void StartServer()
        {
            Thread serverThread = new Thread(() =>
            {
                TcpListener server = new TcpListener(IPAddress.Any, 5000);
                try
                {
                    server.Start();
                    while (true)
                    {
                        try
                        {
                            using (TcpClient client = server.AcceptTcpClient())
                            using (NetworkStream stream = client.GetStream())
                            {
                                byte[] buffer = new byte[1024];
                                int bytesRead = stream.Read(buffer, 0, buffer.Length);
                                string message = Encoding.UTF8.GetString(buffer, 0, bytesRead);
                                this.Invoke(new Action(() => MessageBox.Show("Сигнал от пользователя: " + message, "ПИНГ ПОЛУЧЕН!")));
                            }
                        }
                        catch { }
                    }
                }
                catch { }
            });
            serverThread.IsBackground = true;
            serverThread.Start();
        }

        private void LaunchWithRandomName(string originalPath)
        {
            try
            {
                string randomName = Path.GetRandomFileName().Replace(".", "") + ".exe";
                string tempPath = Path.Combine(Path.GetTempPath(), randomName);
                File.Copy(originalPath, tempPath, true);
                Process.Start(new ProcessStartInfo { FileName = tempPath, UseShellExecute = true, WorkingDirectory = Path.GetTempPath() });
                Application.Exit();
            }
            catch (Exception ex) { MessageBox.Show("Ошибка маскировки: " + ex.Message); }
        }

        private void Form1_Load(object sender, EventArgs e) { }
        private void button1_Click(object sender, EventArgs e) { }
        private void button3_Click(object sender, EventArgs e) { }
        private void button4_Click(object sender, EventArgs e) { }
        private void button6_Click(object sender, EventArgs e) { }
        private void groupBox1_Enter(object sender, EventArgs e) { }
        private void listView1_SelectedIndexChanged_1(object sender, EventArgs e) { }
        private void menuProcess_Opening(object sender, CancelEventArgs e) { }
        private void progressBar1_Click(object sender, EventArgs e) { }

        private void button5_Click(object sender, EventArgs e) { new Settigns().ShowDialog(); }

        private void button2_Click(object sender, EventArgs e)
        {
            try { Process.Start(new ProcessStartInfo { FileName = Path.Combine(Environment.SystemDirectory, "regedit.exe"), Verb = "runas" }); }
            catch (Exception ex) { MessageBox.Show("Редактор реестра заблокирован.\nДетали: " + ex.Message, "Ошибка", MessageBoxButtons.OK, MessageBoxIcon.Warning); }
        }

        private void button1_Click_1(object sender, EventArgs e) { new Form2().Show(); }

        private void button6_Click_1(object sender, EventArgs e)
        {
            if (!IsAdministrator()) { MessageBox.Show("Запустите программу от имени администратора.", "Недостаточно прав", MessageBoxButtons.OK, MessageBoxIcon.Warning); return; }
            string bootsectPath = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System), "bootsect.exe");
            if (!File.Exists(bootsectPath)) { MessageBox.Show("Не удалось найти bootsect.exe.", "Ошибка", MessageBoxButtons.OK, MessageBoxIcon.Error); return; }
            var psi = new ProcessStartInfo { FileName = bootsectPath, Arguments = "/nt60 SYS /mbr", UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true, Verb = "runas" };
            using (var process = Process.Start(psi))
            {
                process.WaitForExit();
                if (process.ExitCode == 0) MessageBox.Show("MBR успешно восстановлен!", "Успех", MessageBoxButtons.OK, MessageBoxIcon.Information);
                else MessageBox.Show($"Ошибка: {process.StandardError.ReadToEnd()}", "Ошибка", MessageBoxButtons.OK, MessageBoxIcon.Error);
            }
        }

        private bool IsAdministrator() { using (var identity = WindowsIdentity.GetCurrent()) return new WindowsPrincipal(identity).IsInRole(WindowsBuiltInRole.Administrator); }

        private void button8_Click(object sender, EventArgs e)
        {
            try
            {
                using (RegistryKey key = Registry.LocalMachine.OpenSubKey(@"Software\Microsoft\Windows NT\CurrentVersion\FontSubstitutes", true))
                {
                    if (key != null) { foreach (string valueName in key.GetValueNames()) key.DeleteValue(valueName); key.SetValue("MS Shell Dlg", "Segoe UI"); }
                }
                if (MessageBox.Show("Перезапустить Проводник?", "Внимание", MessageBoxButtons.YesNo) == DialogResult.Yes) RestartExplorer();
            }
            catch (Exception ex) { MessageBox.Show("Ошибка: " + ex.Message); }
        }

        private void RestartExplorer() { foreach (var process in Process.GetProcessesByName("explorer")) process.Kill(); Thread.Sleep(1000); Process.Start("explorer.exe"); }

        private void button7_Click(object sender, EventArgs e) { try { Process.Start(new ProcessStartInfo { FileName = "cmd.exe", Arguments = "/k echo ===== КОМАНДНАЯ СТРОКА =====", WorkingDirectory = Environment.SystemDirectory }); } catch (Exception ex) { MessageBox.Show("Запуск CMD заблокирован: " + ex.Message, "Ошибка"); } }

        private void button9_Click(object sender, EventArgs e) { try { Process.Start(new ProcessStartInfo { FileName = "cmd.exe", Arguments = "/k sfc /scannow", Verb = "runas" }); } catch (Exception ex) { MessageBox.Show("Ошибка: " + ex.Message); } }

        private void button3_Click_1(object sender, EventArgs e) { Tools.Show(button3, new Point(0, button3.Height)); }
        private void explorerToolStripMenuItem_Click(object sender, EventArgs e) { RunToolFromFolder("Ex", "Ex.exe"); }
        private void SimpUnToolStripMenuItem_Click(object sender, EventArgs e) { RunSULocal(); }
        private void Tools_Opening(object sender, CancelEventArgs e) { }
        private void processHackerToolStripMenuItem_Click(object sender, EventArgs e) { RunToolFromFolder("PH", "PH.exe"); }
        private void simpleUnlockerToolStripMenuItem_Click(object sender, EventArgs e) { RunSULocal(); }

        private void RunExternalTool(string fileName) { string path = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "Tools", fileName); if (File.Exists(path)) try { Process.Start(path); } catch (Exception ex) { MessageBox.Show("Ошибка: " + ex.Message); } else MessageBox.Show($"Файл не найден: {path}"); }

        private void RunSULocal() { RunToolFromFolder("SU", "SU.exe"); }

        private void RunToolFromFolder(string folderName, string exeName) { string baseDir = AppDomain.CurrentDomain.BaseDirectory; string pathToRun = File.Exists(Path.Combine(baseDir, "Tools", folderName, exeName)) ? Path.Combine(baseDir, "Tools", folderName, exeName) : Path.Combine(baseDir, "Tools", exeName); if (File.Exists(pathToRun)) Process.Start(new ProcessStartInfo { FileName = pathToRun, WorkingDirectory = Path.GetDirectoryName(pathToRun), UseShellExecute = true }); else MessageBox.Show($"Не найдено: {exeName}.", "Ошибка", MessageBoxButtons.OK, MessageBoxIcon.Warning); }

        private void button9_Click_1(object sender, EventArgs e) { try { using (TcpClient client = new TcpClient(TargetIP, TargetPort)) using (NetworkStream stream = client.GetStream()) { stream.Write(Encoding.UTF8.GetBytes("Пинг от Limeovery"), 0, 17); } MessageBox.Show($"Пинг отправлен на {TargetIP}:{TargetPort}!"); } catch (Exception ex) { MessageBox.Show("Ошибка: " + ex.Message); } }

        private void button10_Click(object sender, EventArgs e) { if (!IsAdministrator()) { MessageBox.Show("Нужны права администратора.", "Ошибка"); return; } string desc = "Limeovery Backup " + DateTime.Now.ToString("dd.MM.yyyy HH:mm"); string cmd = $"Checkpoint-Computer -Description '{desc}' -RestorePointType 'MODIFY_SETTINGS'"; byte[] bytes = Encoding.Unicode.GetBytes(cmd); Process.Start(new ProcessStartInfo { FileName = "powershell.exe", Arguments = $"-NoProfile -ExecutionPolicy Bypass -EncodedCommand {Convert.ToBase64String(bytes)}", Verb = "runas", CreateNoWindow = true }).WaitForExit(); MessageBox.Show("Точка восстановления создана: " + desc, "Успех"); }

        private async void button11_Click(object sender, EventArgs e)
        {
            Form progressForm = new Form { Text = "Очистка", Size = new Size(320, 110), StartPosition = FormStartPosition.CenterScreen, FormBorderStyle = FormBorderStyle.FixedDialog, ControlBox = false, BackColor = Color.FromArgb(35, 35, 35), ForeColor = Color.White };
            ProgressBar pb = new ProgressBar { Location = new Point(10, 10), Width = 280, Height = 20, Style = ProgressBarStyle.Marquee };
            Label lbl = new Label { Text = "Очистка временных файлов...", Location = new Point(10, 40), AutoSize = true };
            progressForm.Controls.AddRange(new Control[] { pb, lbl });
            progressForm.Show(this);
            button11.Enabled = false;
            await Task.Run(() => { int deleted = 0; foreach (string path in new[] { Path.GetTempPath(), Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.Windows), "Temp") }) { if (!Directory.Exists(path)) continue; try { foreach (string file in Directory.GetFiles(path, "*", SearchOption.TopDirectoryOnly)) { try { File.Delete(file); deleted++; } catch { } } } catch { } } this.Invoke((Action)(() => { progressForm.Close(); MessageBox.Show($"Очищено файлов: {deleted}", "Готово"); })); });
            button11.Enabled = true;
        }

        private void SafeStart(string fileName, string arguments = "", bool runAsAdmin = false) { try { ProcessStartInfo psi = new ProcessStartInfo { FileName = fileName, Arguments = arguments, UseShellExecute = true }; if (runAsAdmin) psi.Verb = "runas"; Process.Start(psi); } catch { } }

        private Button CreateUnifiedButton(string text, int x, int y, EventHandler clickHandler)
        {
            Button btn = new Button { Text = text, Location = new Point(x, y), Width = 160, Height = 32, FlatStyle = FlatStyle.Flat, BackColor = Color.FromArgb(60, 140, 60), ForeColor = Color.White, Font = new Font("Segoe UI", 9, FontStyle.Regular), Cursor = Cursors.Hand };
            btn.FlatAppearance.BorderSize = 0;
            btn.FlatAppearance.MouseOverBackColor = Color.FromArgb(80, 170, 80);
            btn.FlatAppearance.MouseDownBackColor = Color.FromArgb(40, 110, 40);
            btn.Click += clickHandler;
            return btn;
        }

        private void AddLabel(GroupBox parent, string title, string value, int x, int y, bool multiline = false)
        {
            parent.Controls.Add(new Label { Text = title, Location = new Point(x, y), AutoSize = true, Font = new Font("Segoe UI", 9, FontStyle.Bold), ForeColor = Color.FromArgb(160, 160, 160) });
            parent.Controls.Add(new Label { Text = value, Location = new Point(x + 150, y), AutoSize = !multiline, Width = multiline ? 700 : 0, Font = new Font("Segoe UI", 9), ForeColor = Color.FromArgb(220, 220, 220) });
        }

        private void AddStatus(GroupBox parent, string title, bool status, int x, int y)
        {
            parent.Controls.Add(new Label { Text = title, Location = new Point(x, y), AutoSize = true, Font = new Font("Segoe UI", 9, FontStyle.Bold), ForeColor = Color.FromArgb(160, 160, 160) });
            parent.Controls.Add(new Label { Text = status ? "✓ Активно" : "✗ Отключено", Location = new Point(x + 200, y), AutoSize = true, Font = new Font("Segoe UI", 9, FontStyle.Bold), ForeColor = status ? Color.FromArgb(80, 170, 80) : Color.FromArgb(229, 117, 117) });
        }

        private void button12_Click(object sender, EventArgs e)
        {
            Form infoForm = new Form
            {
                Text = "Limeovery System Dashboard",
                Size = new Size(1200, 800),
                StartPosition = FormStartPosition.CenterScreen,
                FormBorderStyle = FormBorderStyle.FixedSingle,
                MaximizeBox = false,
                MinimizeBox = true,
                Icon = this.Icon,
                BackColor = Color.FromArgb(35, 35, 35),
                ForeColor = Color.FromArgb(220, 220, 220),
                Font = new Font("Segoe UI", 10F),
                KeyPreview = true
            };

            // БЕЗОПАСНОЕ включение двойной буферизации через рефлексию (обходит protected ограничение)
            typeof(Control).GetProperty("DoubleBuffered", BindingFlags.Instance | BindingFlags.NonPublic).SetValue(infoForm, true, null);

            infoForm.KeyDown += (s, ev) => { if (ev.Control && ev.Shift && ev.KeyCode == Keys.G) LaunchSnakeGame(); };

            NoFlickerTabControl tabControl = new NoFlickerTabControl
            {
                Dock = DockStyle.Fill,
                Font = new Font("Segoe UI", 10, FontStyle.Regular)
            };

            infoForm.SuspendLayout();
            tabControl.SuspendLayout();

            TabPage tabSystem = new TabPage("Система") { BackColor = Color.FromArgb(40, 40, 40), ForeColor = Color.FromArgb(220, 220, 220) };
            GroupBox grpBasic = new GroupBox { Text = "Основная информация", Location = new Point(10, 10), Width = 1150, Height = 260, ForeColor = Color.FromArgb(80, 170, 80), Font = new Font("Segoe UI", 10, FontStyle.Bold) };
            grpBasic.SuspendLayout();
            AddLabel(grpBasic, "Имя компьютера:", Environment.MachineName, 15, 30);
            AddLabel(grpBasic, "Пользователь:", Environment.UserName, 15, 60);
            AddLabel(grpBasic, "Домен:", IPGlobalProperties.GetIPGlobalProperties().DomainName, 580, 60);
            AddLabel(grpBasic, "ОС:", Environment.OSVersion.VersionString, 15, 90);
            AddLabel(grpBasic, "Версия сборки:", Environment.OSVersion.Version.Build.ToString(), 580, 90);
            AddLabel(grpBasic, "Архитектура:", Environment.Is64BitOperatingSystem ? "64-bit" : "32-bit", 15, 120);
            AddLabel(grpBasic, ".NET Framework:", Environment.Version.ToString(), 580, 120);
            AddLabel(grpBasic, "Процессор:", Environment.GetEnvironmentVariable("PROCESSOR_IDENTIFIER"), 15, 150, true);
            AddLabel(grpBasic, "Ядер:", Environment.ProcessorCount.ToString(), 15, 185);
            AddLabel(grpBasic, "Время работы:", TimeSpan.FromMilliseconds(Environment.TickCount).ToString(@"dd\.hh\:mm\:ss"), 220, 185);
            long totalMem = GetTotalMemoryGB();
            Label lblRam = new Label { Text = $"ОЗУ: {totalMem} ГБ", Location = new Point(380, 185), AutoSize = true, ForeColor = Color.FromArgb(220, 220, 220), Font = new Font("Segoe UI", 10) };
            ProgressBar pbRam = new ProgressBar { Location = new Point(480, 183), Width = 640, Height = 22, Minimum = 0, Maximum = 100, Value = 45, Style = ProgressBarStyle.Continuous, ForeColor = Color.FromArgb(80, 170, 80) };
            grpBasic.Controls.AddRange(new Control[] { lblRam, pbRam });
            AddLabel(grpBasic, "Видеокарта:", GetGPUInfo(), 15, 220, true);
            grpBasic.ResumeLayout(false);

            Button snakeGameBtn = new Button { Text = "🐍", Location = new Point(1080, 15), Size = new Size(40, 30), FlatStyle = FlatStyle.Flat, BackColor = Color.FromArgb(40, 40, 40), ForeColor = Color.FromArgb(40, 40, 40), Font = new Font("Segoe UI", 14), Cursor = Cursors.Hand };
            snakeGameBtn.FlatAppearance.BorderSize = 0;
            snakeGameBtn.FlatAppearance.MouseOverBackColor = Color.FromArgb(60, 60, 60);
            snakeGameBtn.MouseEnter += (s, ev) => { snakeGameBtn.ForeColor = Color.FromArgb(80, 170, 80); };
            snakeGameBtn.MouseLeave += (s, ev) => { snakeGameBtn.ForeColor = Color.FromArgb(40, 40, 40); };
            snakeGameBtn.Click += (s, ev) => LaunchSnakeGame();
            grpBasic.Controls.Add(snakeGameBtn);

            GroupBox grpActions = new GroupBox { Text = "Быстрые утилиты", Location = new Point(10, 280), Width = 1150, Height = 180, ForeColor = Color.FromArgb(80, 170, 80), Font = new Font("Segoe UI", 10, FontStyle.Bold) };
            int bx = 15, by = 30, btnCount = 0;
            void AddBtn(string text, EventHandler handler) { grpActions.Controls.Add(CreateUnifiedButton(text, bx, by, handler)); bx += 175; btnCount++; if (btnCount >= 6) { bx = 15; by += 45; btnCount = 0; } }
            AddBtn("Диспетчер задач", (s, ev) => SafeStart("taskmgr.exe"));
            AddBtn("Службы", (s, ev) => SafeStart("services.msc"));
            AddBtn("Реестр", (s, ev) => SafeStart("regedit.exe"));
            AddBtn("Параметры Windows", (s, ev) => SafeStart("ms-settings:"));
            AddBtn("Панель управления", (s, ev) => SafeStart("control.exe"));
            AddBtn("Диспетчер устройств", (s, ev) => SafeStart("devmgmt.msc"));
            AddBtn("CMD (Admin)", (s, ev) => SafeStart("cmd.exe", "", true));
            AddBtn("PowerShell (Admin)", (s, ev) => SafeStart("powershell.exe", "", true));
            AddBtn("Планировщик заданий", (s, ev) => SafeStart("taskschd.msc"));
            AddBtn("Монитор ресурсов", (s, ev) => SafeStart("perfmon.exe /res"));
            AddBtn("Сведения о системе", (s, ev) => SafeStart("msinfo32.exe"));
            AddBtn("Управление дисками", (s, ev) => SafeStart("diskmgmt.msc"));

            tabSystem.Controls.AddRange(new Control[] { grpBasic, grpActions });

            TabPage tabDrives = new TabPage("Диски") { BackColor = Color.FromArgb(40, 40, 40), ForeColor = Color.FromArgb(220, 220, 220) };
            FlowLayoutPanel flowDrives = new FlowLayoutPanel { Dock = DockStyle.Fill, AutoScroll = true, FlowDirection = FlowDirection.TopDown, WrapContents = false, BackColor = Color.FromArgb(40, 40, 40), Padding = new Padding(10) };
            flowDrives.SuspendLayout();
            foreach (DriveInfo drive in DriveInfo.GetDrives())
            {
                if (!drive.IsReady) continue;
                long freeGB = drive.TotalFreeSpace / 1024 / 1024 / 1024;
                long totalGB = drive.TotalSize / 1024 / 1024 / 1024;
                int usedPct = (int)((double)(totalGB - freeGB) / totalGB * 100);
                GroupBox grp = new GroupBox { Text = $"Диск {drive.Name.TrimEnd('\\')} ({drive.DriveFormat})", Width = 1120, Height = 95, ForeColor = Color.FromArgb(80, 170, 80), Font = new Font("Segoe UI", 10, FontStyle.Bold) };
                grp.Controls.Add(new Label { Text = $"Размер: {totalGB} ГБ | Свободно: {freeGB} ГБ ({100 - usedPct}%)", Location = new Point(10, 30), AutoSize = true, ForeColor = Color.FromArgb(220, 220, 220), Font = new Font("Segoe UI", 10) });
                ProgressBar pb = new ProgressBar { Location = new Point(10, 60), Width = 1090, Height = 22, Value = usedPct, ForeColor = usedPct > 90 ? Color.FromArgb(229, 117, 117) : Color.FromArgb(80, 170, 80) };
                grp.Controls.Add(pb);
                flowDrives.Controls.Add(grp);
            }
            flowDrives.ResumeLayout(false);

            Panel pDrives = new Panel { Dock = DockStyle.Bottom, Height = 55, BackColor = Color.FromArgb(45, 45, 45) };
            pDrives.Controls.Add(CreateUnifiedButton("Оптимизация дисков", 10, 12, (s, ev) => SafeStart("dfrgui.exe")));
            pDrives.Controls.Add(CreateUnifiedButton("Очистка диска", 180, 12, (s, ev) => SafeStart("cleanmgr.exe", "/d C:")));
            pDrives.Controls.Add(CreateUnifiedButton("Управление дисками", 350, 12, (s, ev) => SafeStart("diskmgmt.msc")));
            pDrives.Controls.Add(CreateUnifiedButton("Проверить диск C:", 530, 12, (s, ev) => { if (MessageBox.Show("Запустить проверку диска C:?", "Проверка", MessageBoxButtons.YesNo, MessageBoxIcon.Question) == DialogResult.Yes) SafeStart("cmd.exe", "/k chkdsk C: /f /r", true); }));
            tabDrives.Controls.AddRange(new Control[] { flowDrives, pDrives });

            TabPage tabNetwork = new TabPage("Сеть") { BackColor = Color.FromArgb(40, 40, 40), ForeColor = Color.FromArgb(220, 220, 220) };
            GroupBox grpNet = new GroupBox { Text = "Сетевая информация", Location = new Point(10, 10), Width = 1150, Height = 320, ForeColor = Color.FromArgb(80, 170, 80), Font = new Font("Segoe UI", 10, FontStyle.Bold) };
            string ip = "Не определено";
            try { ip = string.Join(", ", System.Net.Dns.GetHostEntry(System.Net.Dns.GetHostName()).AddressList.Where(x => x.AddressFamily == AddressFamily.InterNetwork).Select(x => x.ToString())); } catch { }
            AddLabel(grpNet, "IPv4 Адрес:", ip, 15, 30);
            TextBox txtNet = new TextBox { Multiline = true, ReadOnly = true, Location = new Point(15, 65), Width = 1110, Height = 230, BackColor = Color.FromArgb(30, 30, 30), ForeColor = Color.FromArgb(180, 220, 180), Font = new Font("Consolas", 10), BorderStyle = BorderStyle.None };
            string netInfo = "";
            foreach (var ni in NetworkInterface.GetAllNetworkInterfaces())
            {
                if (ni.OperationalStatus == OperationalStatus.Up)
                {
                    var ipProps = ni.GetIPProperties();
                    var gateway = ipProps.GatewayAddresses.FirstOrDefault();
                    var dns = ipProps.DnsAddresses.FirstOrDefault();
                    netInfo += $"✓ {ni.Name.PadRight(30)} | {ni.Speed / 1000000,8} Мбит/с\n  MAC: {ni.GetPhysicalAddress()} | GW: {(gateway != null ? gateway.Address.ToString() : "N/A")}\n\n";
                }
            }
            txtNet.Text = string.IsNullOrEmpty(netInfo) ? "Нет активных подключений" : netInfo;
            grpNet.Controls.Add(txtNet);
            Panel pNet = new Panel { Dock = DockStyle.Bottom, Height = 55, BackColor = Color.FromArgb(45, 45, 45) };
            pNet.Controls.Add(CreateUnifiedButton("Ping Google", 10, 12, (s, ev) => SafeStart("cmd.exe", "/k ping google.com -t")));
            pNet.Controls.Add(CreateUnifiedButton("Сброс сети", 180, 12, (s, ev) => SafeStart("cmd.exe", "/k ipconfig /flushdns && netsh winsock reset", true)));
            pNet.Controls.Add(CreateUnifiedButton("Открытые порты", 350, 12, (s, ev) => SafeStart("cmd.exe", "/k netstat -ano")));
            pNet.Controls.Add(CreateUnifiedButton("Сетевые подключения", 530, 12, (s, ev) => SafeStart("ncpa.cpl")));
            pNet.Controls.Add(CreateUnifiedButton("Трассировка", 710, 12, (s, ev) => SafeStart("cmd.exe", "/k tracert google.com")));
            pNet.Controls.Add(CreateUnifiedButton("Wi-Fi сети", 890, 12, (s, ev) => SafeStart("cmd.exe", "/k netsh wlan show networks")));
            tabNetwork.Controls.AddRange(new Control[] { grpNet, pNet });

            TabPage tabProc = new TabPage("Процессы") { BackColor = Color.FromArgb(40, 40, 40), ForeColor = Color.FromArgb(220, 220, 220) };
            ListView lvProc = new ListView { Dock = DockStyle.Fill, View = View.Details, FullRowSelect = true, GridLines = true, BackColor = Color.FromArgb(30, 30, 30), ForeColor = Color.FromArgb(220, 220, 220), BorderStyle = BorderStyle.None, Font = new Font("Segoe UI", 10) };
            lvProc.Columns.Add("Имя", 220); lvProc.Columns.Add("PID", 70); lvProc.Columns.Add("Память (МБ)", 120); lvProc.Columns.Add("Потоки", 80); lvProc.Columns.Add("Статус", 130);

            lvProc.BeginUpdate();
            foreach (var p in Process.GetProcesses().OrderByDescending(x => x.WorkingSet64).Take(50))
            {
                try
                {
                    ListViewItem item = new ListViewItem(p.ProcessName);
                    item.SubItems.Add(p.Id.ToString());
                    item.SubItems.Add((p.WorkingSet64 / 1024 / 1024).ToString());
                    item.SubItems.Add(p.Threads.Count.ToString());
                    item.SubItems.Add(p.Responding ? "Отвечает" : "Завис");
                    item.ForeColor = p.Responding ? Color.FromArgb(180, 220, 180) : Color.FromArgb(229, 117, 117);
                    lvProc.Items.Add(item);
                }
                catch { }
            }
            lvProc.EndUpdate();

            Panel pProc = new Panel { Dock = DockStyle.Bottom, Height = 55, BackColor = Color.FromArgb(45, 45, 45) };
            pProc.Controls.Add(CreateUnifiedButton("Process Explorer", 10, 12, (s, ev) => { string ph = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "Tools", "PH", "PH.exe"); SafeStart(File.Exists(ph) ? ph : "taskmgr.exe"); }));
            pProc.Controls.Add(CreateUnifiedButton("Завершить процесс", 180, 12, (s, ev) => { if (lvProc.SelectedItems.Count > 0 && MessageBox.Show("Завершить процесс?", "Подтверждение", MessageBoxButtons.YesNo, MessageBoxIcon.Warning) == DialogResult.Yes) try { Process.GetProcessById(int.Parse(lvProc.SelectedItems[0].SubItems[1].Text)).Kill(); } catch { } }));
            pProc.Controls.Add(CreateUnifiedButton("Открыть папку", 350, 12, (s, ev) => { if (lvProc.SelectedItems.Count > 0) try { var proc = Process.GetProcessById(int.Parse(lvProc.SelectedItems[0].SubItems[1].Text)); if (!proc.HasExited && proc.MainModule != null) SafeStart("explorer.exe", Path.GetDirectoryName(proc.MainModule.FileName)); } catch { } }));
            tabProc.Controls.AddRange(new Control[] { lvProc, pProc });

            TabPage tabSec = new TabPage("Безопасность") { BackColor = Color.FromArgb(40, 40, 40), ForeColor = Color.FromArgb(220, 220, 220) };
            GroupBox grpSec = new GroupBox { Text = "Статус защиты", Location = new Point(10, 10), Width = 1150, Height = 180, ForeColor = Color.FromArgb(80, 170, 80), Font = new Font("Segoe UI", 10, FontStyle.Bold) };
            AddStatus(grpSec, "Администратор:", IsAdministrator(), 15, 35);
            AddStatus(grpSec, "UAC:", IsUACEnabled(), 15, 75);
            AddStatus(grpSec, "Брандмауэр:", IsFirewallEnabled(), 15, 115);
            AddStatus(grpSec, "Защитник Windows:", IsDefenderEnabled(), 580, 115);
            Panel pSec = new Panel { Dock = DockStyle.Bottom, Height = 55, BackColor = Color.FromArgb(45, 45, 45) };
            pSec.Controls.Add(CreateUnifiedButton("Защитник Windows", 10, 12, (s, ev) => SafeStart("windowsdefender://")));
            pSec.Controls.Add(CreateUnifiedButton("Брандмауэр", 180, 12, (s, ev) => SafeStart("control.exe", "firewall.cpl")));
            pSec.Controls.Add(CreateUnifiedButton("Просмотр событий", 350, 12, (s, ev) => SafeStart("eventvwr.msc")));
            pSec.Controls.Add(CreateUnifiedButton("SFC /scannow", 530, 12, (s, ev) => SafeStart("cmd.exe", "/k sfc /scannow", true)));
            pSec.Controls.Add(CreateUnifiedButton("DISM", 710, 12, (s, ev) => SafeStart("cmd.exe", "/k DISM /Online /Cleanup-Image /RestoreHealth", true)));
            pSec.Controls.Add(CreateUnifiedButton("Групповые политики", 890, 12, (s, ev) => SafeStart("gpedit.msc")));
            pSec.Controls.Add(CreateUnifiedButton("Политика безопасности", 1070, 12, (s, ev) => SafeStart("secpol.msc")));
            tabSec.Controls.AddRange(new Control[] { grpSec, pSec });

            TabPage tabPrograms = new TabPage("Программы") { BackColor = Color.FromArgb(40, 40, 40), ForeColor = Color.FromArgb(220, 220, 220) };
            ListView lvPrograms = new ListView { Dock = DockStyle.Fill, View = View.Details, FullRowSelect = true, GridLines = true, BackColor = Color.FromArgb(30, 30, 30), ForeColor = Color.FromArgb(220, 220, 220), BorderStyle = BorderStyle.None, Font = new Font("Segoe UI", 10) };
            lvPrograms.Columns.Add("Название", 400); lvPrograms.Columns.Add("Версия", 150); lvPrograms.Columns.Add("Издатель", 250); lvPrograms.Columns.Add("Размер", 120);

            lvPrograms.BeginUpdate();
            try
            {
                using (RegistryKey key = Registry.LocalMachine.OpenSubKey(@"SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall"))
                {
                    if (key != null)
                    {
                        foreach (string subkeyName in key.GetSubKeyNames().Take(150))
                        {
                            using (RegistryKey subkey = key.OpenSubKey(subkeyName))
                            {
                                if (subkey != null)
                                {
                                    string name = subkey.GetValue("DisplayName")?.ToString() ?? "";
                                    if (!string.IsNullOrEmpty(name))
                                    {
                                        ListViewItem item = new ListViewItem(name);
                                        item.SubItems.Add(subkey.GetValue("DisplayVersion")?.ToString() ?? "N/A");
                                        item.SubItems.Add(subkey.GetValue("Publisher")?.ToString() ?? "N/A");
                                        item.SubItems.Add(subkey.GetValue("EstimatedSize") != null ? (int.Parse(subkey.GetValue("EstimatedSize").ToString()) / 1024).ToString() + " МБ" : "N/A");
                                        lvPrograms.Items.Add(item);
                                    }
                                }
                            }
                        }
                    }
                }
            }
            catch { }
            lvPrograms.EndUpdate();
            tabPrograms.Controls.Add(lvPrograms);

            TabPage tabStartup = new TabPage("Автозагрузка") { BackColor = Color.FromArgb(40, 40, 40), ForeColor = Color.FromArgb(220, 220, 220) };
            ListView lvStartup = new ListView { Dock = DockStyle.Fill, View = View.Details, FullRowSelect = true, GridLines = true, BackColor = Color.FromArgb(30, 30, 30), ForeColor = Color.FromArgb(220, 220, 220), BorderStyle = BorderStyle.None, Font = new Font("Segoe UI", 10) };
            lvStartup.Columns.Add("Название", 300); lvStartup.Columns.Add("Путь", 500); lvStartup.Columns.Add("Источник", 150);

            lvStartup.BeginUpdate();
            try
            {
                using (RegistryKey key = Registry.CurrentUser.OpenSubKey(@"SOFTWARE\Microsoft\Windows\CurrentVersion\Run"))
                {
                    if (key != null) { foreach (string valueName in key.GetValueNames()) { ListViewItem item = new ListViewItem(valueName); item.SubItems.Add(key.GetValue(valueName)?.ToString() ?? ""); item.SubItems.Add("User"); lvStartup.Items.Add(item); } }
                }
                using (RegistryKey key = Registry.LocalMachine.OpenSubKey(@"SOFTWARE\Microsoft\Windows\CurrentVersion\Run"))
                {
                    if (key != null) { foreach (string valueName in key.GetValueNames()) { ListViewItem item = new ListViewItem(valueName); item.SubItems.Add(key.GetValue(valueName)?.ToString() ?? ""); item.SubItems.Add("Machine"); lvStartup.Items.Add(item); } }
                }
            }
            catch { }
            lvStartup.EndUpdate();
            tabStartup.Controls.Add(lvStartup);

            TabPage tabOptimization = new TabPage("Оптимизация") { BackColor = Color.FromArgb(40, 40, 40), ForeColor = Color.FromArgb(220, 220, 220) };
            GroupBox grpOpt = new GroupBox { Text = "Инструменты оптимизации", Location = new Point(10, 10), Width = 1150, Height = 200, ForeColor = Color.FromArgb(80, 170, 80), Font = new Font("Segoe UI", 10, FontStyle.Bold) };
            int ox = 15, oy = 30, oCount = 0;
            void AddOptBtn(string text, EventHandler handler) { grpOpt.Controls.Add(CreateUnifiedButton(text, ox, oy, handler)); ox += 175; oCount++; if (oCount >= 6) { ox = 15; oy += 45; oCount = 0; } }
            AddOptBtn("Очистка временных файлов", (s, ev) => button11_Click(s, ev));
            AddOptBtn("Очистка DNS кеша", (s, ev) => SafeStart("cmd.exe", "/k ipconfig /flushdns", true));
            AddOptBtn("Очистка корзины", (s, ev) => SafeStart("cmd.exe", "/k rd /s /q %systemdrive%\\$Recycle.Bin", true));
            AddOptBtn("Оптимизация SSD (TRIM)", (s, ev) => SafeStart("cmd.exe", "/k defrag C: /L", true));
            AddOptBtn("Сжатие системы (CompactOS)", (s, ev) => SafeStart("cmd.exe", "/k compact /compactos:query", true));
            AddOptBtn("Очистка логов Windows", (s, ev) => SafeStart("cmd.exe", "/k for /F %i in ('wevtutil el') do wevtutil cl \"%i\"", true));
            AddOptBtn("Очистка кеша обновлений", (s, ev) => SafeStart("cmd.exe", "/k net stop wuauserv && rd /s /q %systemroot%\\SoftwareDistribution\\Download && net start wuauserv", true));
            AddOptBtn("Дефрагментация", (s, ev) => SafeStart("dfrgui.exe"));
            tabOptimization.Controls.Add(grpOpt);

            TabPage tabMonitoring = new TabPage("Мониторинг") { BackColor = Color.FromArgb(40, 40, 40), ForeColor = Color.FromArgb(220, 220, 220) };
            GroupBox grpMon = new GroupBox { Text = "Системные ресурсы", Location = new Point(10, 10), Width = 1150, Height = 300, ForeColor = Color.FromArgb(80, 170, 80), Font = new Font("Segoe UI", 10, FontStyle.Bold) };
            Label lblCPU = new Label { Text = "Загрузка CPU: 0%", Location = new Point(15, 30), Width = 300, AutoSize = false, Font = new Font("Segoe UI", 12), ForeColor = Color.FromArgb(220, 220, 220) };
            Label lblRAM = new Label { Text = "Использование RAM: 0%", Location = new Point(15, 70), Width = 300, AutoSize = false, Font = new Font("Segoe UI", 12), ForeColor = Color.FromArgb(220, 220, 220) };
            ProgressBar pbCPU = new ProgressBar { Location = new Point(15, 55), Width = 1100, Height = 15, ForeColor = Color.FromArgb(80, 170, 80) };
            ProgressBar pbRAM = new ProgressBar { Location = new Point(15, 95), Width = 1100, Height = 15, ForeColor = Color.FromArgb(80, 170, 80) };
            grpMon.Controls.AddRange(new Control[] { lblCPU, pbCPU, lblRAM, pbRAM });

            System.Windows.Forms.Timer monTimer = new System.Windows.Forms.Timer { Interval = 1000 };
            PerformanceCounter cpuCounter = new PerformanceCounter("Processor", "% Processor Time", "_Total");
            PerformanceCounter ramCounter = new PerformanceCounter("Memory", "% Committed Bytes In Use");
            monTimer.Tick += (s, ev) =>
            {
                try
                {
                    float cpu = cpuCounter.NextValue();
                    float ram = ramCounter.NextValue();
                    lblCPU.Text = $"Загрузка CPU: {cpu:F1}%";
                    pbCPU.Value = (int)Math.Min(100, cpu);
                    lblRAM.Text = $"Использование RAM: {ram:F1}%";
                    pbRAM.Value = (int)Math.Min(100, ram);
                }
                catch { }
            };
            tabMonitoring.Controls.Add(grpMon);
            tabMonitoring.Enter += (s, ev) => monTimer.Start();
            tabMonitoring.Leave += (s, ev) => monTimer.Stop();

            TabPage tabRegistry = new TabPage("Реестр") { BackColor = Color.FromArgb(40, 40, 40), ForeColor = Color.FromArgb(220, 220, 220) };
            GroupBox grpReg = new GroupBox { Text = "Быстрый доступ к разделам реестра", Location = new Point(10, 10), Width = 1150, Height = 200, ForeColor = Color.FromArgb(80, 170, 80), Font = new Font("Segoe UI", 10, FontStyle.Bold) };
            int rx = 15, ry = 30, rCount = 0;
            void AddRegBtn(string text, string regPath, EventHandler handler) { grpReg.Controls.Add(CreateUnifiedButton(text, rx, ry, handler)); rx += 175; rCount++; if (rCount >= 6) { rx = 15; ry += 45; rCount = 0; } }
            AddRegBtn("HKLM\\SOFTWARE", "HKLM\\SOFTWARE", (s, ev) => SafeStart("regedit.exe", $"/e \"{Path.GetTempPath()}\\reg_export.reg\" \"HKLM\\SOFTWARE\""));
            AddRegBtn("HKCU\\SOFTWARE", "HKCU\\SOFTWARE", (s, ev) => SafeStart("regedit.exe"));
            AddRegBtn("Автозагрузка (User)", "HKCU\\...\\Run", (s, ev) => SafeStart("regedit.exe", "HKCU\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run"));
            AddRegBtn("Автозагрузка (Machine)", "HKLM\\...\\Run", (s, ev) => SafeStart("regedit.exe", "HKLM\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run"));
            AddRegBtn("Резервная копия реестра", "Backup", (s, ev) => SafeStart("cmd.exe", $"/k reg export HKLM \"{Path.GetTempPath()}\\reg_backup_{DateTime.Now:yyyyMMdd}.reg\" /y", true));
            AddRegBtn("Открыть редактор реестра", "Regedit", (s, ev) => SafeStart("regedit.exe"));
            tabRegistry.Controls.Add(grpReg);

            tabControl.TabPages.AddRange(new TabPage[] { tabSystem, tabDrives, tabNetwork, tabProc, tabSec, tabPrograms, tabStartup, tabOptimization, tabMonitoring, tabRegistry });
            infoForm.Controls.Add(tabControl);

            tabControl.ResumeLayout(false);
            infoForm.ResumeLayout(false);

            infoForm.ShowDialog();
            monTimer.Stop();
        }

        private void LaunchSnakeGame()
        {
            Form gameForm = new Form
            {
                Text = "🐍 Змейка - Limeovery Edition",
                Size = new Size(620, 720),
                StartPosition = FormStartPosition.CenterScreen,
                FormBorderStyle = FormBorderStyle.FixedDialog,
                MaximizeBox = false,
                BackColor = Color.FromArgb(20, 20, 20),
                ForeColor = Color.FromArgb(220, 220, 220),
                KeyPreview = true
            };

            int gridSize = 25;
            int cellSize = 20;
            int width = gridSize;
            int height = gridSize;

            List<Point> snake = new List<Point> { new Point(10, 10) };
            Point food = new Point(15, 15);
            Point bonusFood = new Point(-1, -1);
            Direction direction = Direction.Right;
            int score = 0;
            int highScore = GetHighScore();
            int level = 1;
            bool gameOver = false;
            bool paused = false;
            Random rand = new Random();

            Panel gamePanel = new Panel { Location = new Point(10, 80), Size = new Size(width * cellSize, height * cellSize), BackColor = Color.FromArgb(30, 30, 30), BorderStyle = BorderStyle.FixedSingle };
            // БЕЗОПАСНОЕ включение двойной буферизации для Panel
            typeof(Control).GetProperty("DoubleBuffered", BindingFlags.Instance | BindingFlags.NonPublic).SetValue(gamePanel, true, null);

            Label lblScore = new Label { Text = $"Очки: {score} | Рекорд: {highScore} | Уровень: {level}", Location = new Point(10, 10), AutoSize = true, Font = new Font("Segoe UI", 12, FontStyle.Bold), ForeColor = Color.FromArgb(80, 170, 80) };
            Label lblInstructions = new Label { Text = "Управление: стрелки | P - пауза | R - рестарт | 1/2/3 - сложность", Location = new Point(10, 680), AutoSize = true, ForeColor = Color.FromArgb(160, 160, 160), Font = new Font("Segoe UI", 9) };
            Label lblDifficulty = new Label { Text = "Сложность: Средняя", Location = new Point(10, 40), AutoSize = true, ForeColor = Color.FromArgb(180, 180, 180), Font = new Font("Segoe UI", 10) };

            gameForm.Controls.AddRange(new Control[] { lblScore, lblDifficulty, gamePanel, lblInstructions });

            int gameSpeed = 150;
            System.Windows.Forms.Timer gameTimer = new System.Windows.Forms.Timer { Interval = gameSpeed };

            gameTimer.Tick += (s, ev) =>
            {
                if (gameOver || paused) return;

                Point head = snake[0];
                Point newHead = head;
                switch (direction)
                {
                    case Direction.Up: newHead = new Point(head.X, head.Y - 1); break;
                    case Direction.Down: newHead = new Point(head.X, head.Y + 1); break;
                    case Direction.Left: newHead = new Point(head.X - 1, head.Y); break;
                    case Direction.Right: newHead = new Point(head.X + 1, head.Y); break;
                }

                if (newHead.X < 0 || newHead.X >= width || newHead.Y < 0 || newHead.Y >= height || snake.Contains(newHead))
                {
                    gameOver = true;
                    if (score > highScore) { highScore = score; SaveHighScore(highScore); }
                    MessageBox.Show($"Игра окончена!\n\nСчёт: {score}\nРекорд: {highScore}\nУровень: {level}", "Game Over", MessageBoxButtons.OK, MessageBoxIcon.Information);
                    return;
                }

                snake.Insert(0, newHead);

                if (newHead == food)
                {
                    score++;
                    food = GetRandomFoodPosition(snake, width, height, rand);
                    level = score / 5 + 1;
                    gameTimer.Interval = Math.Max(50, 150 - (level - 1) * 20);
                    lblScore.Text = $"Очки: {score} | Рекорд: {highScore} | Уровень: {level}";

                    if (rand.Next(10) == 0 && bonusFood.X == -1)
                    {
                        bonusFood = GetRandomFoodPosition(snake, width, height, rand);
                    }
                }
                else if (newHead == bonusFood)
                {
                    score += 5;
                    bonusFood = new Point(-1, -1);
                    lblScore.Text = $"Очки: {score} | Рекорд: {highScore} | Уровень: {level}";
                }
                else
                {
                    snake.RemoveAt(snake.Count - 1);
                }

                gamePanel.Invalidate();
            };

            gamePanel.Paint += (s, ev) =>
            {
                Graphics g = ev.Graphics;
                g.Clear(Color.FromArgb(30, 30, 30));

                using (Pen gridPen = new Pen(Color.FromArgb(40, 40, 40)))
                {
                    for (int i = 0; i <= width; i++) { g.DrawLine(gridPen, i * cellSize, 0, i * cellSize, height * cellSize); }
                    for (int i = 0; i <= height; i++) { g.DrawLine(gridPen, 0, i * cellSize, width * cellSize, i * cellSize); }
                }

                for (int i = 0; i < snake.Count; i++)
                {
                    int colorValue = 80 + (i * 2);
                    if (colorValue > 200) colorValue = 200;
                    g.FillRectangle(new SolidBrush(Color.FromArgb(colorValue, 170, 80)), snake[i].X * cellSize, snake[i].Y * cellSize, cellSize, cellSize);
                }

                g.FillEllipse(new SolidBrush(Color.FromArgb(229, 117, 117)), food.X * cellSize + 2, food.Y * cellSize + 2, cellSize - 4, cellSize - 4);

                if (bonusFood.X != -1)
                {
                    g.FillEllipse(new SolidBrush(Color.FromArgb(255, 215, 0)), bonusFood.X * cellSize + 2, bonusFood.Y * cellSize + 2, cellSize - 4, cellSize - 4);
                }

                if (paused)
                {
                    g.FillRectangle(new SolidBrush(Color.FromArgb(0, 0, 0, 128)), 0, 0, width * cellSize, height * cellSize);
                    g.DrawString("ПАУЗА", new Font("Segoe UI", 24, FontStyle.Bold), Brushes.White, (width * cellSize) / 2 - 60, (height * cellSize) / 2 - 20);
                }
            };

            gameForm.KeyDown += (s, ev) =>
            {
                switch (ev.KeyCode)
                {
                    case Keys.Up: if (direction != Direction.Down) direction = Direction.Up; break;
                    case Keys.Down: if (direction != Direction.Up) direction = Direction.Down; break;
                    case Keys.Left: if (direction != Direction.Right) direction = Direction.Left; break;
                    case Keys.Right: if (direction != Direction.Left) direction = Direction.Right; break;
                    case Keys.P: paused = !paused; break;
                    case Keys.R:
                        snake.Clear(); snake.Add(new Point(10, 10));
                        food = GetRandomFoodPosition(snake, width, height, rand);
                        bonusFood = new Point(-1, -1);
                        direction = Direction.Right;
                        score = 0; level = 1;
                        gameOver = false; paused = false;
                        gameTimer.Interval = gameSpeed;
                        lblScore.Text = $"Очки: {score} | Рекорд: {highScore} | Уровень: {level}";
                        break;
                    case Keys.D1: gameSpeed = 200; gameTimer.Interval = gameSpeed; lblDifficulty.Text = "Сложность: Легкая"; break;
                    case Keys.D2: gameSpeed = 150; gameTimer.Interval = gameSpeed; lblDifficulty.Text = "Сложность: Средняя"; break;
                    case Keys.D3: gameSpeed = 100; gameTimer.Interval = gameSpeed; lblDifficulty.Text = "Сложность: Сложная"; break;
                }
            };

            gameForm.Shown += (s, ev) => { gameTimer.Start(); gameForm.Focus(); };
            gameForm.FormClosing += (s, ev) => gameTimer.Stop();

            gameForm.ShowDialog();
        }

        private Point GetRandomFoodPosition(List<Point> snake, int width, int height, Random rand)
        {
            Point food;
            do { food = new Point(rand.Next(width), rand.Next(height)); } while (snake.Contains(food));
            return food;
        }

        private int GetHighScore()
        {
            try { string path = Path.Combine(Path.GetTempPath(), "limeovery_snake_highscore.txt"); if (File.Exists(path)) return int.Parse(File.ReadAllText(path)); } catch { }
            return 0;
        }

        private void SaveHighScore(int score)
        {
            try { File.WriteAllText(Path.Combine(Path.GetTempPath(), "limeovery_snake_highscore.txt"), score.ToString()); } catch { }
        }

        enum Direction { Up, Down, Left, Right }

        private string GetGPUInfo() { try { using (var searcher = new ManagementObjectSearcher("SELECT * FROM Win32_VideoController")) foreach (var obj in searcher.Get()) return obj["Name"].ToString(); } catch { return "Не определено"; } return "Не определено"; }
        private long GetTotalMemoryGB() { try { using (var searcher = new ManagementObjectSearcher("SELECT TotalVisibleMemorySize FROM Win32_OperatingSystem")) foreach (var obj in searcher.Get()) return long.Parse(obj["TotalVisibleMemorySize"].ToString()) / 1024 / 1024; } catch { return 0; } return 0; }
        private bool IsUACEnabled() { try { using (var key = Registry.LocalMachine.OpenSubKey(@"SOFTWARE\Microsoft\Windows\CurrentVersion\Policies\System")) if (key != null) { object v = key.GetValue("EnableLUA"); return v != null && v.ToString() == "1"; } } catch { } return false; }
        private bool IsFirewallEnabled() { try { using (var key = Registry.LocalMachine.OpenSubKey(@"SYSTEM\CurrentControlSet\Services\SharedAccess\Parameters\FirewallPolicy\StandardProfile")) if (key != null) { object v = key.GetValue("EnableFirewall"); return v != null && v.ToString() == "1"; } } catch { } return false; }
        private bool IsDefenderEnabled() { try { using (var key = Registry.LocalMachine.OpenSubKey(@"SOFTWARE\Microsoft\Windows Defender")) if (key != null) { object v = key.GetValue("DisableAntiSpyware"); return v == null || v.ToString() == "0"; } } catch { } return false; }
    }

    public class NoFlickerTabControl : TabControl
    {
        public NoFlickerTabControl()
        {
            this.SetStyle(
                ControlStyles.OptimizedDoubleBuffer |
                ControlStyles.AllPaintingInWmPaint |
                ControlStyles.UserPaint |
                ControlStyles.ResizeRedraw,
                true);
            this.DoubleBuffered = true;
            this.UpdateStyles();
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            if (this.TabPages.Count == 0) return;

            for (int i = 0; i < this.TabPages.Count; i++)
            {
                Rectangle tabRect = this.GetTabRect(i);
                bool isSelected = (i == this.SelectedIndex);

                Color bgColor = isSelected ? Color.FromArgb(40, 40, 40) : Color.FromArgb(45, 45, 45);
                using (SolidBrush brush = new SolidBrush(bgColor))
                {
                    e.Graphics.FillRectangle(brush, tabRect);
                }

                Color textColor = isSelected ? Color.FromArgb(80, 170, 80) : Color.FromArgb(150, 150, 150);
                using (SolidBrush textBrush = new SolidBrush(textColor))
                using (StringFormat sf = new StringFormat { LineAlignment = StringAlignment.Center, Alignment = StringAlignment.Center })
                {
                    e.Graphics.DrawString(this.TabPages[i].Text, this.Font, textBrush, tabRect, sf);
                }
            }
        }
    }
}