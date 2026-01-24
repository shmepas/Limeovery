using Microsoft.Win32;
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Data;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Linq;
using System.Net;
using System.Net.Sockets;
using System.Security.Principal;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using System.Windows.Forms;

namespace Limeovery
{
    public partial class Form1 : Form
    {
        // ПЕРЕМЕННЫЕ ДЛЯ НАСТРОЕК (Доступны для Form3)
        public static string TargetIP = "192.168.3.19";
        public static int TargetPort = 5000;

        public Form1()
        {
            InitializeComponent();
            StartServer();
        }

        private void StartServer()
        {
            Thread serverThread = new Thread(() =>
            {
                // Используем порт 5000 для прослушивания (как в твоем оригинале)
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

                                this.Invoke(new Action(() =>
                                {
                                    MessageBox.Show("Сигнал от пользователя: " + message, "ПИНГ ПОЛУЧЕН!");
                                }));
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

        private void Form1_Load(object sender, EventArgs e) { }

        private void button1_Click(object sender, EventArgs e) { }

        private void button5_Click(object sender, EventArgs e)
        {
            Settigns settingsWin = new Settigns(); 
            settingsWin.ShowDialog();
        }

        private void button3_Click(object sender, EventArgs e) { }

        private void button4_Click(object sender, EventArgs e) { }

        private void button6_Click(object sender, EventArgs e) { }

        private void button2_Click(object sender, EventArgs e)
        {
            try
            {
                ProcessStartInfo regInfo = new ProcessStartInfo();
                regInfo.FileName = Path.Combine(Environment.SystemDirectory, "regedit.exe");
                regInfo.Verb = "runas";
                Process.Start(regInfo);
            }
            catch (Exception ex)
            {
                MessageBox.Show("Внимание! Редактор реестра заблокирован или поврежден.\n\n" + "Попробуйте сначала нажать кнопку 'РАЗБЛОКИРОВАТЬ ВСЁ'.\n\n" + "Детали ошибки: " + ex.Message, "Критическая ошибка", MessageBoxButtons.OK, MessageBoxIcon.Warning);
            }
        }

        private void button1_Click_1(object sender, EventArgs e)
        {
            Form2 taskManager = new Form2();
            taskManager.Show();
        }


        private void button6_Click_1(object sender, EventArgs e)
        {
            if (!IsAdministrator())
            {
                MessageBox.Show(
                    "Запустите программу от имени администратора.",
                    "Недостаточно прав",
                    MessageBoxButtons.OK,
                    MessageBoxIcon.Warning
                );
                return;
            }

            string bootsectPath = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System), "bootsect.exe");

            if (!File.Exists(bootsectPath))
            {
                MessageBox.Show(
                    "Не удалось найти ни bcdboot.exe, ни bootsect.exe.\n\n" +
                    "Решение:\n" +
                    "1. Загрузитесь с установочного носителя Windows\n" +
                    "2. Откройте «Командную строку» из меню восстановления\n" +
                    "3. Выполните команды:\n" +
                    "   bootrec /fixmbr\n" +
                    "   bootrec /fixboot\n" +
                    "   bootrec /rebuildbcd",
                    "Ошибка",
                    MessageBoxButtons.OK,
                    MessageBoxIcon.Error
                );
                return;
            }

            var psi = new ProcessStartInfo
            {
                FileName = bootsectPath,
                Arguments = "/nt60 SYS /mbr",
                UseShellExecute = false,
                CreateNoWindow = true,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                Verb = "runas"
            };

            using (var process = Process.Start(psi))
            {
                string output = process.StandardOutput.ReadToEnd();
                string error = process.StandardError.ReadToEnd();
                process.WaitForExit();

                if (process.ExitCode == 0)
                {
                    MessageBox.Show(
                        "MBR успешно восстановлен!\n\n" +
                        "Перезагрузите компьютер. Работает только для BIOS-систем.",
                        "Успех",
                        MessageBoxButtons.OK,
                        MessageBoxIcon.Information
                    );
                }
                else
                {
                    MessageBox.Show(
                        $"Ошибка при восстановлении MBR:\n{error}",
                        "Ошибка",
                        MessageBoxButtons.OK,
                        MessageBoxIcon.Error
                    );
                }
            }
        }

        private bool IsAdministrator()
        {
            using (var identity = WindowsIdentity.GetCurrent())
            {
                var principal = new WindowsPrincipal(identity);
                return principal.IsInRole(WindowsBuiltInRole.Administrator);
            }
        }
        private void button8_Click(object sender, EventArgs e)
        {
            try
            {
                string fontSubstKey = @"Software\Microsoft\Windows NT\CurrentVersion\FontSubtites";
                using (RegistryKey key = Registry.LocalMachine.OpenSubKey(fontSubstKey, true))
                {
                    if (key != null)
                    {
                        foreach (string valueName in key.GetValueNames())
                        {
                            key.DeleteValue(valueName);
                        }
                        key.SetValue("MS Shell Dlg", "Segoe UI");
                        MessageBox.Show("Настройки шрифтов сброшены", "Готово");
                    }
                }
                if (MessageBox.Show("Для применения изменений нужно перезапустить Проводник. Сделать это сейчас?", "Внимание", MessageBoxButtons.YesNo) == DialogResult.Yes)
                {
                    RestartExplorer();
                }
            }
            catch (Exception ex)
            {
                MessageBox.Show("Ошибка при работе с реестром: " + ex.Message);
            }
        }

        private void RestartExplorer()
        {
            try
            {
                foreach (var process in Process.GetProcessesByName("explorer"))
                {
                    process.Kill();
                }
                Thread.Sleep(1000);
                Process.Start("explorer.exe");
            }
            catch
            {
                Process.Start("explorer.exe");
            }
        }

        private void button7_Click(object sender, EventArgs e)
        {
            try
            {
                ProcessStartInfo psi = new ProcessStartInfo();
                psi.FileName = "cmd.exe";
                psi.Arguments = "/k echo ===== КОМАНДНАЯ СТРОКА =====";
                psi.WorkingDirectory = Environment.GetEnvironmentVariable("SystemRoot") + Environment.SystemDirectory;
                Process.Start(psi);
            }
            catch (Exception ex)
            {
                MessageBox.Show("Внимание! Запуск CMD заблокирован системой или вирусом.\n" + ex.Message, "Ошибка восстановления", MessageBoxButtons.OK, MessageBoxIcon.Error);
            }
        }

        private void button9_Click(object sender, EventArgs e)
        {
            try
            {
                MessageBox.Show("Сейчас запустится сканирование файлов. Это может занять 10-20 минут. Не закрывайте открывшееся окно!", "Инфо");
                ProcessStartInfo repairInfo = new ProcessStartInfo();
                repairInfo.FileName = "cmd.exe";
                repairInfo.Arguments = "/k sfc /scannow";
                repairInfo.Verb = "runas";
                Process.Start(repairInfo);
            }
            catch (Exception ex)
            {
                MessageBox.Show("Ошибка при запуске исправления: " + ex.Message);
            }
        }

        private void groupBox1_Enter(object sender, EventArgs e) { }

        private void button3_Click_1(object sender, EventArgs e)
        {
            Tools.Show(button3, new Point(0, button3.Height));
        }

        private void explorerToolStripMenuItem_Click(object sender, EventArgs e)
        {
            ExtractAndRun(Properties.Resources.Ex, "Ex.exe");
        }

        private void Tools_Opening(object sender, CancelEventArgs e) { }

        private void processHackerToolStripMenuItem_Click(object sender, EventArgs e)
        {
            ExtractAndRun(Properties.Resources.PH, "PH.exe");
        }

        private void RunExternalTool(string fileName)
        {
            string baseDirectory = AppDomain.CurrentDomain.BaseDirectory;
            string toolDashPath = Path.Combine(baseDirectory, "Tools", fileName);

            if (File.Exists(toolDashPath))
            {
                try { Process.Start(toolDashPath); }
                catch (Exception ex) { MessageBox.Show($"Ошибка запуска {fileName}: " + ex.Message); }
            }
            else
            {
                MessageBox.Show($"Файл не найден по пути: {toolDashPath}\nСоздайте папку Tools и положите туда программу.");
            }
        }

        private void ExtractAndRun(byte[] resourceBytes, string fileName)
        {
            try
            {
                string tempFolder = Path.Combine(Path.GetTempPath(), "SystemMedicTools");
                if (!Directory.Exists(tempFolder)) Directory.CreateDirectory(tempFolder);
                string fullPath = Path.Combine(tempFolder, fileName);
                File.WriteAllBytes(fullPath, resourceBytes);
                Process.Start(fullPath);
            }
            catch (Exception ex)
            {
                MessageBox.Show("Не удалось извлечь утилиту: " + ex.Message);
            }
        }

        // КНОПКА ОТПРАВИТЬ ПИНГ (ИСПОЛЬЗУЕТ НАСТРОЙКИ)
        private void button9_Click_1(object sender, EventArgs e)
        {
            try
            {
                // Используем статические переменные вместо жесткого IP
                using (TcpClient client = new TcpClient(TargetIP, TargetPort))
                using (NetworkStream stream = client.GetStream())
                {
                    byte[] data = Encoding.UTF8.GetBytes("Пинг от Limeovery");
                    stream.Write(data, 0, data.Length);
                }
                MessageBox.Show($"Пинг успешно отправлен на {TargetIP}:{TargetPort}!");
            }
            catch (Exception ex)
            {
                MessageBox.Show("Не удалось достучаться: " + ex.Message);
            }
        }

        private void listView1_SelectedIndexChanged_1(object sender, EventArgs e) { }

        private void menuProcess_Opening(object sender, CancelEventArgs e) { }

        private void progressBar1_Click(object sender, EventArgs e)
        {

        }
    }
}