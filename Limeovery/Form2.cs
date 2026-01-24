using System;
using System.Diagnostics;
using System.Drawing;
using System.Windows.Forms;

namespace Limeovery
{
    public partial class Form2 : Form
    {
        public Form2()
        {
            InitializeComponent();
            RefreshProcessList();
        }

        private void RefreshProcessList()
        {
            SearchProcesses(textBox1.Text);
        }
        private void SearchProcesses(string searchTerm)
        {
            listView1.Items.Clear();
            searchTerm = searchTerm.ToLower();

            Process[] processes = Process.GetProcesses();

            listView1.BeginUpdate();
            foreach (Process p in processes)
            {
                string name = p.ProcessName.ToLower();
                string pid = p.Id.ToString();

                if (name.Contains(searchTerm) || pid.Contains(searchTerm))
                {
                    ListViewItem item = new ListViewItem(p.ProcessName);
                    item.SubItems.Add(p.Id.ToString());

                    if (string.IsNullOrEmpty(p.MainWindowTitle))
                    {
                        item.ForeColor = Color.DarkSlateGray;
                    }

                    listView1.Items.Add(item);
                }
            }
            listView1.EndUpdate();
        }

        private void обновитьСписокToolStripMenuItem_Click(object sender, EventArgs e)
        {
            RefreshProcessList();
        }

        private void завершитьПроцессToolStripMenuItem_Click(object sender, EventArgs e)
        {
            if (listView1.SelectedItems.Count > 0)
            {
                int pid = int.Parse(listView1.SelectedItems[0].SubItems[1].Text);
                try
                {
                    Process p = Process.GetProcessById(pid);
                    p.Kill();
                    MessageBox.Show("Процесс уничтожен!");
                    RefreshProcessList();
                }
                catch (Exception ex)
                {
                    MessageBox.Show("Ошибка: " + ex.Message);
                }
            }
        }

        private void listView1_SelectedIndexChanged(object sender, EventArgs e)
        {
            if (listView1.SelectedItems.Count > 0)
            {
                try
                {
                    int pid = int.Parse(listView1.SelectedItems[0].SubItems[1].Text);
                    Process p = Process.GetProcessById(pid);
                    this.Text = $"Процесс: {p.ProcessName} | Запущен: {p.StartTime.ToLongTimeString()}";
                }
                catch
                {
                    this.Text = "System Medic - Доступ ограничен";
                }
            }
        }

        // Исправленное событие для поиска
        private void textBox1_TextChanged(object sender, EventArgs e)
        {
            // Передаем текст из самого textBox1 в метод поиска
            SearchProcesses(textBox1.Text);
        }

        private void Form2_Load(object sender, EventArgs e)
        {
            // Можно добавить фокус на поиск при открытии
            textBox1.Focus();
        }
    }
}