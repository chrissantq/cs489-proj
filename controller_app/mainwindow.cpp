#include "mainwindow.h"
#include "./ui_mainwindow.h"

#include "controlwindow.h"

#include <Qt>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
    ui->setupUi(this);
}

MainWindow::~MainWindow()
{
    delete ui;
}

void MainWindow::on_settings_button_clicked()
{
    auto *controlWindow = new ControlWindow();
    controlWindow->setAttribute(Qt::WA_DeleteOnClose);
    controlWindow->show();
    close();
}
