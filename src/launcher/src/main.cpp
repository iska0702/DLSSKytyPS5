#include "mainDialog.h"
#include "launcherTheme.h"

#include "common/u59Preset.h"

#include <QApplication>
#include <QDebug>
#include <QDir>
#include <QProcessEnvironment>

#include <filesystem>
#include <vector>

// The experimental archive ships u59-preset.json next to the launcher. Loading it here gives a
// direct launch the same environment as Launch-U59.ps1. The file is looked for next to the launcher
// and then one folder up (where FindInterpreter also looks for the emulator). When it is missing or
// broken, the launcher keeps the current environment, logs where it looked and returns the warning
// for the main window: the emulator then runs with its code defaults.
static QString ApplyBundledPreset() {
	const QDir                         app_dir(QApplication::applicationDirPath());
	std::vector<std::filesystem::path> dirs {std::filesystem::path(app_dir.absolutePath().toStdU16String())};
	if (QDir parent = app_dir; parent.cdUp()) {
		dirs.emplace_back(parent.absolutePath().toStdU16String());
		if (QDir parent_tools = parent; parent_tools.cd("tools")) {
			dirs.emplace_back(parent_tools.absolutePath().toStdU16String());
		}
		if (QDir grand = parent; grand.cdUp()) {
			if (QDir grand_tools = grand; grand_tools.cd("tools")) {
				dirs.emplace_back(grand_tools.absolutePath().toStdU16String());
			}
		}
	}
	if (QDir app_tools = app_dir; app_tools.cd("tools")) {
		dirs.emplace_back(app_tools.absolutePath().toStdU16String());
	}

	auto result = Common::U59Preset::Load(dirs);
	if (result.status != Common::U59Preset::Status::Loaded) {
		// Fallback: parse embedded default preset so emulator never degrades to unoptimized built-in defaults
		std::string parse_err;
		if (Common::U59Preset::Parse(Common::U59Preset::BundledFallbackPresetJson(), result.entries, parse_err)) {
			result.status = Common::U59Preset::Status::Loaded;
		} else {
			const auto warning = QString::fromStdString(Common::U59Preset::Describe(result));
			qWarning().noquote() << warning;
			return warning;
		}
	}

	for (const QString& key : QProcessEnvironment::systemEnvironment().keys()) {
		if (key.startsWith("KYTY_", Qt::CaseInsensitive) ||
		    key.startsWith("TRACY_", Qt::CaseInsensitive)) {
			qunsetenv(key.toLocal8Bit().constData());
		}
	}
	for (const auto& entry : result.entries) {
		qputenv(entry.key.c_str(), QByteArray::fromStdString(entry.value));
	}
	return {};
}
#include <QIcon>

int main(int argc, char* argv[]) {
	QApplication a(argc, argv);
	// Title bars and taskbars get the bolder small-size artwork.
	const QIcon small_art(QStringLiteral(":/icons/kyty-small.svg"));
	const QIcon large_art(QStringLiteral(":/icons/kyty.svg"));
	QIcon       icon;
	for (int size: {16, 24, 32, 48, 64}) {
		icon.addPixmap(small_art.pixmap(QSize(size, size), 1.0));
	}
	for (int size: {128, 256}) {
		icon.addPixmap(large_art.pixmap(QSize(size, size), 1.0));
	}
	QApplication::setWindowIcon(icon);
	const QString preset_warning = ApplyBundledPreset();
	LauncherTheme::Initialize(a);

	MainDialog w;
	if (!preset_warning.isEmpty()) {
		w.SetPresetWarning(preset_warning);
	}

	w.emit Start();

	w.show();

	return QApplication::exec();
}
