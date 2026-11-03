#include "shaderCompilerDialog.h"

#include "cheatFile.h"
#include "configuration.h"
#include "gameContent.h"

#include <QBoxLayout>
#include <QCloseEvent>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QFont>
#include <QLabel>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>

ShaderCompilerDialog::ShaderCompilerDialog(const Configuration& info, const QString& runtime_dir,
                                           const QIcon& icon, QWidget* parent)
    : QDialog(parent, Qt::WindowTitleHint | Qt::WindowSystemMenuHint | Qt::WindowCloseButtonHint),
      m_info(info),
      m_runtime_dir(runtime_dir),
      m_game_icon(icon),
      m_progress_regex(QStringLiteral(R"(Shader precompile:\s+(\d+)/(\d+)\s+\((\d+)%\))")),
      m_done_regex(QStringLiteral(R"(Shader precompile:\s+(?:done|all shaders and pipelines precompiled))")),
      m_empty_regex(QStringLiteral(R"(Shader precompile:\s+no journaled shaders)")) {
	const QString game_title = m_info.name.isEmpty() ? m_info.title_id : m_info.name;
	setWindowTitle(tr("Compile shaders — %1").arg(game_title));
	setMinimumWidth(580);
	setMinimumHeight(400);

	auto* main_layout = new QVBoxLayout(this);
	main_layout->setSpacing(12);
	main_layout->setContentsMargins(16, 16, 16, 16);

	// Header
	auto* header_layout = new QHBoxLayout();
	header_layout->setSpacing(14);

	if (!m_game_icon.isNull()) {
		auto* icon_label = new QLabel(this);
		icon_label->setPixmap(m_game_icon.pixmap(56, 56));
		icon_label->setFixedSize(56, 56);
		header_layout->addWidget(icon_label);
	}

	auto* title_layout = new QVBoxLayout();
	title_layout->setSpacing(2);
	auto* title_label = new QLabel(game_title, this);
	QFont title_font  = title_label->font();
	title_font.setPointSize(13);
	title_font.setBold(true);
	title_label->setFont(title_font);

	auto* id_label = new QLabel(
	    tr("Title ID: %1").arg(m_info.title_id.isEmpty() ? tr("Unknown") : m_info.title_id), this);
	id_label->setStyleSheet(QStringLiteral("color: palette(mid); font-size: 11px;"));

	title_layout->addWidget(title_label);
	title_layout->addWidget(id_label);
	header_layout->addLayout(title_layout);
	header_layout->addStretch();
	main_layout->addLayout(header_layout);

	// Status label
	m_status_label = new QLabel(tr("Initializing Vulkan runtime and preparing shader precompiler..."), this);
	QFont status_font = m_status_label->font();
	status_font.setWeight(QFont::Medium);
	m_status_label->setFont(status_font);
	m_status_label->setWordWrap(true);
	main_layout->addWidget(m_status_label);

	// Progress bar
	m_progress_bar = new QProgressBar(this);
	m_progress_bar->setRange(0, 100);
	m_progress_bar->setValue(0);
	m_progress_bar->setTextVisible(true);
	m_progress_bar->setFormat(tr("%p%"));
	m_progress_bar->setFixedHeight(24);
	main_layout->addWidget(m_progress_bar);

	// Log text edit
	m_log_edit = new QPlainTextEdit(this);
	m_log_edit->setReadOnly(true);
	m_log_edit->setMaximumBlockCount(2000);
	QFont mono_font(QStringLiteral("Courier New"), 9);
	mono_font.setStyleHint(QFont::Monospace);
	m_log_edit->setFont(mono_font);
	m_log_edit->setStyleSheet(QStringLiteral(
	    "QPlainTextEdit { background-color: #181818; color: #cfcfcf; border: 1px solid palette(mid); "
	    "border-radius: 4px; padding: 4px; }"));
	main_layout->addWidget(m_log_edit, 1);

	// Bottom action buttons
	auto* button_layout = new QHBoxLayout();
	button_layout->addStretch();
	m_cancel_button = new QPushButton(tr("Cancel"), this);
	m_cancel_button->setMinimumWidth(90);
	connect(m_cancel_button, &QPushButton::clicked, this, &ShaderCompilerDialog::OnCancelClicked);
	button_layout->addWidget(m_cancel_button);
	main_layout->addLayout(button_layout);

	m_process = new QProcess(this);
	connect(m_process, &QProcess::readyReadStandardOutput, this,
	        &ShaderCompilerDialog::OnReadyReadStandardOutput);
	connect(m_process, &QProcess::readyReadStandardError, this,
	        &ShaderCompilerDialog::OnReadyReadStandardError);
	connect(m_process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
	        &ShaderCompilerDialog::OnProcessFinished);

	StartPrecompilation();
}

ShaderCompilerDialog::~ShaderCompilerDialog() {
	if (m_process != nullptr) {
		if (m_process->state() != QProcess::NotRunning) {
			m_process->kill();
			m_process->waitForFinished(1000);
		}
	}
}

void ShaderCompilerDialog::StartPrecompilation() {
	QString emulator_path = QDir(m_runtime_dir).filePath(QStringLiteral("kyty_emulator.exe"));
#ifndef _WIN32
	if (!QFile::exists(emulator_path)) {
		emulator_path = QDir(m_runtime_dir).filePath(QStringLiteral("kyty_emulator"));
	}
#endif
	if (!QFile::exists(emulator_path)) {
		emulator_path =
		    QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("kyty_emulator.exe"));
#ifndef _WIN32
		if (!QFile::exists(emulator_path)) {
			emulator_path =
			    QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("kyty_emulator"));
		}
#endif
	}

	if (!QFile::exists(emulator_path)) {
		m_status_label->setText(tr("Error: kyty_emulator executable was not found."));
		m_cancel_button->setText(tr("Close"));
		m_completed = true;
		return;
	}

	QStringList args;
	QString     game = m_info.basedir;
	if (!m_info.elf.isEmpty() && !GameContent::IsArchive(m_info.basedir)) {
		game = QDir(m_info.basedir).filePath(m_info.elf);
	}
	args << QStringLiteral("--game") << game;
	if (!m_info.addons_dir.isEmpty()) {
		args << QStringLiteral("--addcont-dir") << m_info.addons_dir;
	}
	const auto patch_plan = Cheats::PlanPath(m_info.title_id);
	if (QFileInfo::exists(patch_plan)) {
		args << QStringLiteral("--game-patch") << patch_plan;
	}
	if (m_info.gpu_index >= 0) {
		args << QStringLiteral("--gpu") << QString::number(m_info.gpu_index);
	}
	args << QStringLiteral("--no-splash");
	args << QStringLiteral("--precompile-shaders");
	args << QStringLiteral("--precompile-only");

	QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
	env.insert(QStringLiteral("KYTY_SHADER_PRECOMPILE"), QStringLiteral("1"));
	env.insert(QStringLiteral("KYTY_SHADER_PRECOMPILE_WAIT"), QStringLiteral("1"));
	env.insert(QStringLiteral("KYTY_PIPELINE_PREWARM_COMPUTE"), QStringLiteral("1"));
	env.insert(QStringLiteral("KYTY_SHADER_PRECOMPILE_ONLY"), QStringLiteral("1"));
	env.insert(QStringLiteral("KYTY_SHADER_PRECOMPILE_THREADS"), QStringLiteral("16"));
	env.insert(QStringLiteral("KYTY_PIPELINE_PREWARM_THREADS"), QStringLiteral("8"));

	m_process->setProcessEnvironment(env);
	m_process->setWorkingDirectory(QFileInfo(emulator_path).absolutePath());
	m_process->setProgram(emulator_path);
	m_process->setArguments(args);
	m_process->start();

	if (!m_process->waitForStarted(5000)) {
		m_status_label->setText(
		    tr("Error: Failed to launch emulator process (%1)").arg(m_process->errorString()));
		m_cancel_button->setText(tr("Close"));
		m_completed = true;
	}
}

void ShaderCompilerDialog::OnReadyReadStandardOutput() {
	if (m_process == nullptr) return;
	const QByteArray data = m_process->readAllStandardOutput();
	const QString    text = QString::fromUtf8(data);
	const auto       lines = text.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
	for (const auto& line : lines) {
		ProcessLine(line.trimmed());
	}
}

void ShaderCompilerDialog::OnReadyReadStandardError() {
	if (m_process == nullptr) return;
	const QByteArray data = m_process->readAllStandardError();
	const QString    text = QString::fromUtf8(data);
	const auto       lines = text.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
	for (const auto& line : lines) {
		ProcessLine(line.trimmed());
	}
}

void ShaderCompilerDialog::ProcessLine(const QString& line) {
	if (line.isEmpty()) return;
	m_log_edit->appendPlainText(line);

	auto match = m_progress_regex.match(line);
	if (match.hasMatch()) {
		int current = match.captured(1).toInt();
		int total   = match.captured(2).toInt();
		int percent = match.captured(3).toInt();
		m_progress_bar->setValue(percent);
		m_progress_bar->setFormat(QStringLiteral("%1 / %2 (%3%)").arg(current).arg(total).arg(percent));
		m_status_label->setText(
		    tr("Precompiling shaders into Vulkan pipeline cache: %1 / %2 (%3%)...")
		        .arg(current)
		        .arg(total)
		        .arg(percent));
		return;
	}

	if (m_empty_regex.match(line).hasMatch()) {
		m_status_label->setText(
		    tr("No journaled shaders found yet. Shaders will be recorded automatically when playing the game."));
		m_progress_bar->setValue(100);
		m_progress_bar->setFormat(tr("No shaders recorded"));
		return;
	}

	if (m_done_regex.match(line).hasMatch()) {
		m_status_label->setText(
		    tr("All shaders and compute pipelines compiled and saved to disk/driver cache!"));
		m_progress_bar->setValue(100);
		m_progress_bar->setFormat(tr("100% Completed"));
		return;
	}
}

void ShaderCompilerDialog::OnProcessFinished(int exit_code, QProcess::ExitStatus exit_status) {
	m_completed = true;
	m_cancel_button->setText(tr("Close"));
	m_cancel_button->setDefault(true);

	if (exit_status == QProcess::NormalExit && exit_code == 0) {
		m_progress_bar->setValue(100);
		if (!m_progress_bar->format().contains(QStringLiteral("No shaders"))) {
			m_progress_bar->setFormat(tr("100% Completed"));
			m_status_label->setText(
			    tr("Shader precompilation finished successfully! Stutters and freezes prevented."));
		}
	} else if (exit_status == QProcess::CrashExit) {
		m_status_label->setText(
		    tr("Process crashed or terminated prematurely (exit code: %1).").arg(exit_code));
	} else {
		m_status_label->setText(tr("Process completed with exit code %1.").arg(exit_code));
	}
}

void ShaderCompilerDialog::OnCancelClicked() {
	if (!m_completed && m_process != nullptr && m_process->state() != QProcess::NotRunning) {
		m_status_label->setText(tr("Canceling shader precompilation..."));
		m_process->kill();
		m_process->waitForFinished(2000);
	}
	accept();
}

void ShaderCompilerDialog::closeEvent(QCloseEvent* event) {
	if (!m_completed && m_process != nullptr && m_process->state() != QProcess::NotRunning) {
		m_process->kill();
		m_process->waitForFinished(2000);
	}
	event->accept();
}
