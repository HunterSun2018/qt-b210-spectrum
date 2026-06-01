#include "MainWindow.h"

#include <QDoubleSpinBox>
#include <QComboBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QWidget>

#include "SdrWorker.h"
#include "SpectrumWidget.h"
#include "WaterfallWidget.h"

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent),
      m_worker(std::make_unique<SdrWorker>())
{
    buildUi();

    connect(m_startButton, &QPushButton::clicked, this, &MainWindow::startStreaming);
    connect(m_stopButton, &QPushButton::clicked, this, &MainWindow::stopStreaming);
    connect(m_rateSpin, &QDoubleSpinBox::valueChanged, this, &MainWindow::updateSpectrumAxes);
    connect(m_freqSpin, &QDoubleSpinBox::valueChanged, this, &MainWindow::updateSpectrumAxes);

    connect(m_worker.get(), &SdrWorker::spectrumReady, this, &MainWindow::handleSpectrum);
    connect(m_worker.get(), &SdrWorker::statusChanged, this, &MainWindow::handleStatus);
    connect(m_worker.get(), &SdrWorker::errorOccurred, this, &MainWindow::handleError);

    updateSpectrumAxes();
}

MainWindow::~MainWindow()
{
    m_worker->stopStreaming();
    m_worker->wait();
}

void MainWindow::startStreaming()
{
    SdrWorker::Settings settings;
    settings.inputSource = m_sourceCombo->currentData().toInt() == 0
                               ? SdrWorker::InputSource::Usrp
                               : SdrWorker::InputSource::Simulator;
    settings.processorMode = m_processorCombo->currentData().toInt() == 0
                                 ? SdrWorker::ProcessorMode::FloatFft
                                 : SdrWorker::ProcessorMode::Int16Fftw;
    settings.demodMode = static_cast<SdrWorker::DemodMode>(m_demodCombo->currentData().toInt());
    settings.deviceArgs = m_deviceEdit->text();
    settings.sampleRate = m_rateSpin->value();
    settings.centerFreq = m_freqSpin->value();
    settings.gain = m_gainSpin->value();
    settings.squelchDb = m_squelchSpin->value();
    settings.fftSize = static_cast<std::size_t>(m_fftSpin->value());

    m_startButton->setEnabled(false);
    m_stopButton->setEnabled(true);
    m_worker->startStreaming(settings);
}

void MainWindow::stopStreaming()
{
    m_worker->stopStreaming();
    m_startButton->setEnabled(true);
    m_stopButton->setEnabled(false);
}

void MainWindow::handleSpectrum(const QVector<float> &spectrum)
{
    m_spectrumWidget->setSpectrum(spectrum);
    m_waterfallWidget->addSpectrumLine(spectrum);
}

void MainWindow::handleStatus(const QString &status)
{
    m_statusLabel->setText("Status: " + status);
    if (status == "Stopped" || status == "Error")
    {
        m_startButton->setEnabled(true);
        m_stopButton->setEnabled(false);
    }
}

void MainWindow::handleError(const QString &errorText)
{
    QMessageBox::critical(this, "B210 Streaming Error", errorText);
}

void MainWindow::updateSpectrumAxes()
{
    m_spectrumWidget->setFrequencySpan(m_freqSpin->value(), m_rateSpin->value());
}

void MainWindow::buildUi()
{
    setWindowTitle("B210 Spectrum Viewer");
    resize(1280, 860);

    auto *central = new QWidget(this);
    auto *layout = new QVBoxLayout(central);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(12);

    auto *controlBox = new QGroupBox("Receiver Controls", central);
    auto *controlLayout = new QGridLayout(controlBox);

    m_sourceCombo = new QComboBox(controlBox);
    m_sourceCombo->addItem("USRP B210", 0);
    m_sourceCombo->addItem("Simulator", 1);

    m_processorCombo = new QComboBox(controlBox);
    m_processorCombo->addItem("FftProcessor", 0);
    m_processorCombo->addItem("IqFftwProcessor", 1);

    m_demodCombo = new QComboBox(controlBox);
    m_demodCombo->addItem("Off", static_cast<int>(SdrWorker::DemodMode::None));
    m_demodCombo->addItem("FM", static_cast<int>(SdrWorker::DemodMode::FM));
    m_demodCombo->addItem("AM", static_cast<int>(SdrWorker::DemodMode::AM));

    m_deviceEdit = new QLineEdit(controlBox);
    m_deviceEdit->setPlaceholderText("type=b200 or leave empty for auto-discovery");

    m_rateSpin = new QDoubleSpinBox(controlBox);
    m_rateSpin->setRange(1.0e5, 6.0e7);
    m_rateSpin->setDecimals(0);
    m_rateSpin->setSingleStep(1.0e5);
    m_rateSpin->setValue(1.0e6);
    m_rateSpin->setSuffix(" S/s");
    connect(m_rateSpin, &QDoubleSpinBox::valueChanged, this,
            [this](double value)
            {
                m_worker->setSampleRate(value);
            });

    m_freqSpin = new QDoubleSpinBox(controlBox);
    m_freqSpin->setRange(7.0e7, 6.0e9);
    m_freqSpin->setDecimals(0);
    m_freqSpin->setSingleStep(1.0e6);
    m_freqSpin->setValue(103.9e6);
    m_freqSpin->setSuffix(" Hz");
    connect(m_freqSpin, &QDoubleSpinBox::valueChanged, this,
            [this](double value)
            {
                m_worker->setCenterFreq(value);
            });

    m_gainSpin = new QDoubleSpinBox(controlBox);
    m_gainSpin->setRange(0.0, 76.0);
    m_gainSpin->setDecimals(1);
    m_gainSpin->setSingleStep(1.0);
    m_gainSpin->setValue(40.0);
    m_gainSpin->setSuffix(" dB");
    connect(m_gainSpin, &QDoubleSpinBox::valueChanged, this,
            [this](double value)
            {
                m_worker->setGain(value);
            });

    m_squelchSpin = new QDoubleSpinBox(controlBox);
    m_squelchSpin->setRange(-120.0, 0.0);
    m_squelchSpin->setDecimals(1);
    m_squelchSpin->setSingleStep(1.0);
    m_squelchSpin->setValue(-55.0);
    m_squelchSpin->setSuffix(" dBFS");

    m_fftSpin = new QSpinBox(controlBox);
    m_fftSpin->setRange(256, 16384);
    m_fftSpin->setSingleStep(256);
    m_fftSpin->setValue(2048);

    m_startButton = new QPushButton("Start", controlBox);
    m_stopButton = new QPushButton("Stop", controlBox);
    m_stopButton->setEnabled(false);

    m_statusLabel = new QLabel("Status: Idle", controlBox);

    controlLayout->addWidget(new QLabel("Source", controlBox), 0, 0);
    controlLayout->addWidget(m_sourceCombo, 0, 1);
    controlLayout->addWidget(new QLabel("Processor", controlBox), 0, 2);
    controlLayout->addWidget(m_processorCombo, 0, 3);
    controlLayout->addWidget(new QLabel("Demod", controlBox), 1, 0);
    controlLayout->addWidget(m_demodCombo, 1, 1);
    controlLayout->addWidget(new QLabel("Device Args", controlBox), 1, 2);
    controlLayout->addWidget(m_deviceEdit, 1, 3);
    controlLayout->addWidget(new QLabel("Sample Rate", controlBox), 2, 0);
    controlLayout->addWidget(m_rateSpin, 2, 1);
    controlLayout->addWidget(new QLabel("Center Freq", controlBox), 2, 2);
    controlLayout->addWidget(m_freqSpin, 2, 3);
    controlLayout->addWidget(new QLabel("Gain", controlBox), 3, 0);
    controlLayout->addWidget(m_gainSpin, 3, 1);
    controlLayout->addWidget(new QLabel("Squelch", controlBox), 3, 2);
    controlLayout->addWidget(m_squelchSpin, 3, 3);
    controlLayout->addWidget(new QLabel("FFT Size", controlBox), 4, 2);
    controlLayout->addWidget(m_fftSpin, 4, 3);
    controlLayout->addWidget(m_statusLabel, 5, 0, 1, 2);
    controlLayout->addWidget(m_startButton, 5, 2);
    controlLayout->addWidget(m_stopButton, 5, 3);

    m_spectrumWidget = new SpectrumWidget(central);
    m_waterfallWidget = new WaterfallWidget(central);

    layout->addWidget(controlBox);
    layout->addWidget(m_spectrumWidget, 1);
    layout->addWidget(m_waterfallWidget, 1);

    setCentralWidget(central);
}
