using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Data;
using System.Drawing;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
using System.Windows.Forms;

namespace Limeovery
{
    public partial class Settigns : Form
    {
        public Settigns()
        {
            InitializeComponent();
        }

        // Выполняется при открытии окна настроек
        private void Form3_Load(object sender, EventArgs e)
        {
            // Подставляем текущие значения из Form1 в текстовые поля
            textBox1.Text = Form1.TargetIP;
            textBox2.Text = Form1.TargetPort.ToString();
        }

        // Кнопка "Сохранить"
        private void button1_Click(object sender, EventArgs e)
        {
            // 1. Сохраняем IP адрес
            Form1.TargetIP = textBox1.Text;

            // 2. Проверяем и сохраняем Порт (должен быть числом)
            if (int.TryParse(textBox2.Text, out int newPort))
            {
                Form1.TargetPort = newPort;
                MessageBox.Show("Настройки успешно сохранены!", "Система");
                this.Close(); // Закрываем форму после сохранения
            }
            else
            {
                MessageBox.Show("Ошибка: Порт должен быть числовым значением!", "Ошибка");
            }
        }

        // Эти методы можно оставить пустыми, если не нужна проверка "на лету"
        private void textBox1_TextChanged(object sender, EventArgs e) { }
        private void textBox2_TextChanged(object sender, EventArgs e) { }
    }
}