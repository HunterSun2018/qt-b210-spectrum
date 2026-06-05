#pragma once

#include <memory>

#include <QMainWindow>

class QDoubleSpinBox;
class QComboBox;
class QLineEdit;
class QPushButton;
class QLabel;
class QSpinBox;
class SpectrumWidget;
class SdrWorker;
class WaterfallWidget;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

private slots:
    void startStreaming();
    void stopStreaming();
    void handleSpectrum(const QVector<float> &spectrum);
    void handleStatus(const QString &status);
    void handleError(const QString &errorText);
    void updateSpectrumAxes();

private:
    void buildUi();

    QComboBox *m_sourceCombo = nullptr;
    QComboBox *m_rxDboardCombo = nullptr;
    QComboBox *m_rxFrontendCombo = nullptr;
    QComboBox *m_rxAntennaCombo = nullptr;
    QComboBox *m_processorCombo = nullptr;
    QComboBox *m_demodCombo = nullptr;
    QLineEdit *m_deviceEdit = nullptr;
    QDoubleSpinBox *m_rateSpin = nullptr;
    QDoubleSpinBox *m_freqSpin = nullptr;
    QDoubleSpinBox *m_gainSpin = nullptr;
    QDoubleSpinBox *m_squelchSpin = nullptr;
    QComboBox *m_fftSpin = nullptr;
    QPushButton *m_startButton = nullptr;
    QPushButton *m_stopButton = nullptr;
    QLabel *m_statusLabel = nullptr;
    SpectrumWidget *m_spectrumWidget = nullptr;
    WaterfallWidget *m_waterfallWidget = nullptr;

    std::unique_ptr<SdrWorker> m_worker;
};
