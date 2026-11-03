#ifndef SHADER_COMPILER_DIALOG_H
#define SHADER_COMPILER_DIALOG_H

#include <QDialog>
#include <QIcon>
#include <QProcess>
#include <QRegularExpression>

class QLabel;
class QProgressBar;
class QPlainTextEdit;
class QPushButton;
class Configuration;

class ShaderCompilerDialog : public QDialog {
	Q_OBJECT

public:
	ShaderCompilerDialog(const Configuration& info, const QString& runtime_dir, const QIcon& icon = QIcon(),
	                     QWidget* parent = nullptr);
	~ShaderCompilerDialog() override;

protected:
	void closeEvent(QCloseEvent* event) override;

private slots:
	void OnReadyReadStandardOutput();
	void OnReadyReadStandardError();
	void OnProcessFinished(int exit_code, QProcess::ExitStatus exit_status);
	void OnCancelClicked();

private:
	void StartPrecompilation();
	void ProcessLine(const QString& line);

	const Configuration& m_info;
	QString              m_runtime_dir;
	QIcon                m_game_icon;
	QProcess*            m_process = nullptr;

	QLabel*         m_status_label  = nullptr;
	QProgressBar*   m_progress_bar  = nullptr;
	QPlainTextEdit* m_log_edit      = nullptr;
	QPushButton*    m_cancel_button = nullptr;

	bool               m_completed = false;
	QRegularExpression m_progress_regex;
	QRegularExpression m_done_regex;
	QRegularExpression m_empty_regex;
};

#endif // SHADER_COMPILER_DIALOG_H
