#include "configurationEditDialog.h"

#include "common/emulatorConfig.h"
#include "configuration.h"
#include "mandatoryLineEdit.h"
#include <SDL3/SDL.h>

#include <QAbstractItemView>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLayout>
#include <QLineEdit>
#include <QListView>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QResizeEvent>
#include <QSettings>
#include <QSlider>
#include <QSpinBox>
#include <QStyle>
#include <QToolButton>
#include <QTreeView>
#include <QVBoxLayout>
#include <QtAlgorithms>

#if QT_CONFIG(vulkan)
#include <QVulkanInstance>
#include <QVulkanWindow>
#endif

#include "ui_configuration_edit_dialog.h"

constexpr char SETTINGS_CFG_DIALOG[]               = "ConfigurationEditDialog";
constexpr char SETTINGS_CFG_LAST_GEOMETRY[]        = "geometry";
constexpr int  GLOBAL_SETTINGS_GAME_DIRS_MIN_WIDTH = 400;

static void UpdateControllerColorButton(QPushButton* button, const QString& hex) {
	const QColor color(hex);
	if (!color.isValid()) {
		button->setProperty("controllerColor", QString {});
		button->setText(QObject::tr("Custom"));
		button->setStyleSheet({});
		return;
	}
	const auto normalized = color.name(QColor::HexRgb);
	button->setProperty("controllerColor", normalized);
	button->setText(normalized.toUpper());
	button->setStyleSheet(QStringLiteral("background-color: %1; color: %2;")
	                          .arg(normalized, color.lightness() < 128 ? QStringLiteral("white")
	                                                                   : QStringLiteral("black")));
}

const QStringList CONSOLE_LANGUAGE_NAMES = {
    "Japanese",
    "English (United States)",
    "French (France)",
    "Spanish (Spain)",
    "German",
    "Italian",
    "Dutch",
    "Portuguese (Portugal)",
    "Russian",
    "Korean",
    "Chinese (Traditional)",
    "Chinese (Simplified)",
    "Finnish",
    "Swedish",
    "Danish",
    "Norwegian",
    "Polish",
    "Portuguese (Brazil)",
    "English (United Kingdom)",
    "Turkish",
    "Spanish (Latin America)",
    "Arabic",
    "French (Canada)",
    "Czech",
    "Hungarian",
    "Greek",
    "Romanian",
    "Thai",
    "Vietnamese",
    "Indonesian",
};

static QString NormalizeGameDirectory(const QString& dir) {
	const auto trimmed = dir.trimmed();
	if (trimmed.isEmpty()) {
		return {};
	}

	return QDir::cleanPath(QDir(trimmed).absolutePath());
}

static QString GameDirectoryKey(const QString& dir) {
	const auto normalized = NormalizeGameDirectory(dir);
	if (normalized.isEmpty()) {
		return {};
	}

	auto canonical = QFileInfo(normalized).canonicalFilePath();
	if (canonical.isEmpty()) {
		canonical = normalized;
	}
	canonical = QDir::cleanPath(canonical);

#ifdef __linux__
	return canonical;
#else
	return canonical.toCaseFolded();
#endif
}

// The audio mix sliders (rows 1-4 of the Audio group) are global settings like the controller
// group: shown only in the global settings dialog.
struct AudioMixSlider {
	QSlider* slider;
	QLabel*  value;
	int AudioMixSettings::*field;
};

static std::array<AudioMixSlider, 4> AudioMixSliders(Ui::ConfigurationEditDialog& ui) {
	return {{{ui.slider_audio_master, ui.label_audio_master_value, &AudioMixSettings::master},
	         {ui.slider_audio_main, ui.label_audio_main_value, &AudioMixSettings::main},
	         {ui.slider_audio_music, ui.label_audio_music_value, &AudioMixSettings::music},
	         {ui.slider_audio_pad_speaker, ui.label_audio_pad_speaker_value,
	          &AudioMixSettings::pad_speaker}}};
}

static void SetAudioMixVisible(Ui::ConfigurationEditDialog& ui, bool visible) {
	for (int row = 1; row <= 4; row++) {
		ui.audioLayout->setRowVisible(row, visible);
	}
	// The occlusion mode is global too (graphics row 9).
	ui.graphicsLayout->setRowVisible(ui.checkBox_gpu_occlusion, visible);
}

ConfigurationEditDialog::ConfigurationEditDialog(Configuration& info, QWidget* parent)
    : QDialog(parent, Qt::WindowCloseButtonHint), m_ui(new Ui::ConfigurationEditDialog),
      m_info(info) {
	m_ui->setupUi(this);
	SetAudioMixVisible(*m_ui, false);
	for (const auto& mix: AudioMixSliders(*m_ui)) {
		mix.slider->setMaximum(static_cast<int>(Config::MAX_AUDIO_VOLUME));
		connect(mix.slider, &QSlider::valueChanged, this,
		        [label = mix.value](int value) {
			label->setText(ConfigurationEditDialog::tr("%1%").arg(value));
		});
	}
	setMinimumWidth(width());
	InitGameDirectories();
	m_ui->controller_group->setVisible(false);

	connect(m_ui->ok_button, &QPushButton::clicked, this, &ConfigurationEditDialog::save);
	connect(m_ui->cancel_button, &QPushButton::clicked, this, &QDialog::reject);
	connect(m_ui->clear_button, &QPushButton::clicked, this, &ConfigurationEditDialog::clear);
	connect(m_ui->button_controller_color, &QPushButton::clicked, this, [this]() {
		const auto current = m_ui->button_controller_color->property("controllerColor").toString();
		const auto initial =
		    current.isEmpty() ? QColor(QStringLiteral("#0070d1")) : QColor(current);
		const auto color = QColorDialog::getColor(initial, this, tr("DualSense lightbar color"));
		if (color.isValid()) {
			const auto hex = color.name(QColor::HexRgb);
			UpdateControllerColorButton(m_ui->button_controller_color, hex);
			emit PreviewControllerColor(hex);
		}
	});
	connect(m_ui->button_controller_color_reset, &QPushButton::clicked, this, [this]() {
		UpdateControllerColorButton(m_ui->button_controller_color, {});
		emit PreviewControllerColor({});
	});
	connect(m_ui->slider_controller_vibration, &QSlider::valueChanged, this, [this](int value) {
		m_ui->label_controller_vibration_value->setText(tr("%1%").arg(value));
	});
	connect(m_ui->slider_controller_volume, &QSlider::valueChanged, this, [this](int value) {
		m_ui->label_controller_volume_value->setText(tr("%1%").arg(value));
	});
	connect(m_ui->comboBox_shader_log_direction, &QComboBox::currentTextChanged, this,
	        [this](const QString& text) {
		        auto log = TextToEnum<Configuration::LogDirection>(text);
		        m_ui->lineEdit_shader_log_folder->setEnabled(
		            log == Configuration::LogDirection::File);
	        });
	connect(m_ui->checkBox_cmd_dump, &QCheckBox::toggled, this,
	        [this](bool flag) { m_ui->lineEdit_cmd_dump_folder->setEnabled(flag); });
	connect(m_ui->comboBox_printf_direction, &QComboBox::currentTextChanged, this,
	        [this](const QString& text) {
		        auto log = TextToEnum<Configuration::LogDirection>(text);
		        m_ui->lineEdit_printf_file->setEnabled(log == Configuration::LogDirection::File);
	        });
	connect(m_ui->checkBox_dlss_enable, &QCheckBox::toggled, this, [this](bool checked) {
		if (!checked) {
			m_ui->checkBox_dlss_fg_enable->setChecked(false);
		}
		UpdateDLSSUI();
	});
	connect(m_ui->checkBox_dlss_fg_enable, &QCheckBox::toggled, this, [this](bool checked) {
		if (checked && !m_ui->checkBox_dlss_enable->isChecked()) {
			m_ui->checkBox_dlss_enable->setChecked(true);
		}
		UpdateDLSSUI();
	});
	connect(m_ui->comboBox_dlss_fg_mode, &QComboBox::currentIndexChanged, this, [this](int) {
		UpdateDLSSUI();
	});
	connect(m_ui->comboBox_reflex_mode, &QComboBox::currentIndexChanged, this, [this](int) {
		UpdateDLSSUI();
	});
	connect(m_ui->comboBox_screen_resolution, &QComboBox::currentIndexChanged, this, [this](int) {
		UpdateDLSSUI();
	});
	connect(m_ui->slider_dlss_sharpness, &QSlider::valueChanged, this, [this](int value) {
		m_ui->label_dlss_sharpness_value->setText(QString::number(value / 100.0, 'f', 2));
	});
	connect(m_ui->comboBox_gpu, &QComboBox::currentIndexChanged, this, [this](int) {
		UpdateDLSSUI();
	});

#if defined(_WIN32)
	m_directstorage_checkbox = new QCheckBox(tr("DirectStorage File I/O (Experimental)"), this);
	m_directstorage_checkbox->setToolTip(
	    tr("Use Microsoft DirectStorage for file reads. GPU texture streaming and GPU decompression are not integrated."));
	m_ui->compatibilityLayout->addWidget(m_directstorage_checkbox);
#endif

	setSizeGripEnabled(true);

	restoreGeometry(g_last_geometry);

	Init(info);
}

QByteArray ConfigurationEditDialog::g_last_geometry;

ConfigurationEditDialog::~ConfigurationEditDialog() {
	delete m_ui;
}

void ConfigurationEditDialog::WriteSettings(QSettings& s) {
	s.beginGroup(SETTINGS_CFG_DIALOG);

	if (!g_last_geometry.isEmpty()) {
		s.setValue(SETTINGS_CFG_LAST_GEOMETRY, g_last_geometry);
	}

	s.endGroup();
}

void ConfigurationEditDialog::ReadSettings(QSettings& s) {
	s.beginGroup(SETTINGS_CFG_DIALOG);

	g_last_geometry = s.value(SETTINGS_CFG_LAST_GEOMETRY, g_last_geometry).toByteArray();

	s.endGroup();
}

template <class T>
static void ListInit(QComboBox* combo, T value) {
	combo->clear();
	combo->addItems(EnumToList<T>());
	combo->setCurrentText(EnumToText(value));
}

void ConfigurationEditDialog::Init(const Configuration& info) {
	m_ui->lineEdit_user_name->setMaxLength(static_cast<int>(Config::MAX_USER_NAME_LENGTH));
	m_ui->lineEdit_user_name->setText(info.user_name);
	m_ui->spinBox_user_id->setValue(info.user_id);
	UpdateControllerColorButton(m_ui->button_controller_color, info.controller.color);
	m_ui->slider_controller_vibration->setValue(info.controller.vibration_intensity);
	m_ui->slider_controller_volume->setValue(info.controller.speaker_volume);
	for (const auto& mix: AudioMixSliders(*m_ui)) {
		mix.slider->setValue(info.audio_mix.*mix.field);
	}
	m_ui->checkBox_gpu_occlusion->setChecked(info.gpu_occlusion_accurate);
	auto* microphone = m_ui->comboBox_audio_input_device;
	microphone->clear();
	microphone->addItem(tr("None"), QString {});
	microphone->setToolTip(tr("Microphone used by games. None supplies silence."));
	if (SDL_InitSubSystem(SDL_INIT_AUDIO)) {
		int                device_count = 0;
		SDL_AudioDeviceID* devices      = SDL_GetAudioRecordingDevices(&device_count);
		for (int i = 0; i < device_count; i++) {
			if (const auto* device = SDL_GetAudioDeviceName(devices[i]); device != nullptr) {
				const auto name = QString::fromUtf8(device);
				if (microphone->findData(name) < 0) {
					microphone->addItem(name, name);
				}
			}
		}
		SDL_free(devices);
		SDL_QuitSubSystem(SDL_INIT_AUDIO);
	} else {
		microphone->setToolTip(tr("Microphones could not be listed: %1")
		                           .arg(QString::fromUtf8(SDL_GetError())));
	}
	if (microphone->findData(info.audio_input_device) < 0) {
		microphone->addItem(tr("%1 (unavailable)").arg(info.audio_input_device),
		                    info.audio_input_device);
	}
	microphone->setCurrentIndex(microphone->findData(info.audio_input_device));
	ListInit(m_ui->comboBox_screen_resolution, info.screen_resolution);
	ListInit(m_ui->comboBox_present_mode, info.present_mode);
	m_ui->comboBox_gpu->clear();
	m_ui->comboBox_gpu->addItem(tr("Auto"));
	// Keep Auto when Qt is built without Vulkan support.
#if QT_CONFIG(vulkan)
#if defined(__APPLE__)
	if (!qEnvironmentVariableIsSet("QT_VULKAN_LIB")) {
		const auto base    = QCoreApplication::applicationDirPath();
		auto       library = base + "/libMoltenVK.dylib";
		if (!QFileInfo::exists(library)) {
			library = base + "/../Frameworks/libMoltenVK.dylib";
		}
		if (QFileInfo::exists(library)) {
			qputenv("QT_VULKAN_LIB", library.toUtf8());
		}
	}
#endif
	QVulkanInstance instance;
	instance.setApiVersion(QVersionNumber(1, 3, 0));
#if !defined(__APPLE__) && QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
	instance.setFlags(QVulkanInstance::NoPortabilityDrivers);
#endif
	m_has_nvidia_rtx = false;
	m_has_nvidia_fg  = false;
	m_has_nvidia_mfg = false;
	if (instance.create()) {
		QVulkanWindow window;
		window.setVulkanInstance(&instance);
		for (const auto& device: window.availablePhysicalDevices()) {
			QString dev_name = QString::fromUtf8(device.deviceName);
			m_ui->comboBox_gpu->addItem(dev_name);
			if (device.vendorID == 0x10DE) {
				if (dev_name.contains("RTX", Qt::CaseInsensitive) ||
				    dev_name.contains("TITAN", Qt::CaseInsensitive)) {
					m_has_nvidia_rtx = true;
				}
				if (dev_name.contains("RTX 40", Qt::CaseInsensitive) ||
				    dev_name.contains("RTX 50", Qt::CaseInsensitive) ||
				    dev_name.contains("Ada", Qt::CaseInsensitive) ||
				    dev_name.contains("Blackwell", Qt::CaseInsensitive)) {
					m_has_nvidia_fg = true;
				}
				if (dev_name.contains("RTX 50", Qt::CaseInsensitive) ||
				    dev_name.contains("Blackwell", Qt::CaseInsensitive)) {
					m_has_nvidia_mfg = true;
				}
			}
		}
	}
#endif
	m_ui->comboBox_gpu->setCurrentIndex(
	    info.gpu_index >= 0 && info.gpu_index < m_ui->comboBox_gpu->count() - 1 ? info.gpu_index + 1
	                                                                            : 0);
	m_ui->checkBox_fullscreen->setChecked(info.fullscreen_enabled);
	m_ui->checkBox_hide_cursor->setChecked(info.hide_cursor_enabled);
	m_ui->checkBox_readback->setChecked(info.readback_linear_images);
	m_ui->checkBox_tessellation->setChecked(info.tessellation_enabled);
	m_ui->checkBox_trophy_notifications->setChecked(info.trophy_enabled);
	m_ui->spinBox_vblank_frequency->setValue(info.vblank_frequency);
	m_ui->comboBox_console_language->clear();
	m_ui->comboBox_console_language->addItems(CONSOLE_LANGUAGE_NAMES);
	m_ui->comboBox_console_language->setCurrentIndex(
	    info.console_language >= 0 && info.console_language < CONSOLE_LANGUAGE_NAMES.size()
	        ? info.console_language
	        : Configuration::DEFAULT_CONSOLE_LANGUAGE);
	m_ui->checkBox_shader_validation->setChecked(info.shader_validation_enabled);
	m_ui->checkBox_vulkan_validation->setChecked(info.vulkan_validation_enabled);
	m_ui->checkBox_renderdoc_capture->setChecked(info.renderdoc_enabled);
	m_ui->checkBox_amd_cpu->setChecked(info.amd_cpu_enabled);
#if defined(__APPLE__)
	m_ui->checkBox_amd_cpu->setVisible(false);
#endif
#if defined(_WIN32)
	m_ui->checkBox_red_zone_protection->setChecked(info.red_zone_protection_enabled);
	if (m_directstorage_checkbox != nullptr) {
		m_directstorage_checkbox->setChecked(info.directstorage_enabled);
	}
#else
	m_ui->checkBox_red_zone_protection->setVisible(false);
#endif
	ListInit(m_ui->comboBox_shader_optimization_type, info.shader_optimization_type);
	ListInit(m_ui->comboBox_shader_log_direction, info.shader_log_direction);
	m_ui->lineEdit_shader_log_folder->setText(info.shader_log_folder);
	m_ui->lineEdit_shader_log_folder->setEnabled(info.shader_log_direction ==
	                                             Configuration::LogDirection::File);
	m_ui->checkBox_cmd_dump->setChecked(info.command_buffer_dump_enabled);
	m_ui->lineEdit_cmd_dump_folder->setText(info.command_buffer_dump_folder);
	m_ui->lineEdit_cmd_dump_folder->setEnabled(info.command_buffer_dump_enabled);
	ListInit(m_ui->comboBox_printf_direction, info.printf_direction);
	m_ui->lineEdit_printf_file->setText(info.printf_output_file);
	m_ui->lineEdit_printf_file->setEnabled(info.printf_direction ==
	                                       Configuration::LogDirection::File);
	m_ui->checkBox_profiler->setChecked(info.profiler_enabled);

	m_ui->slider_dlss_sharpness->setValue(
	    qBound(0, static_cast<int>(std::round(info.dlss_sharpness * 100.0f)), 100));
	m_ui->label_dlss_sharpness_value->setText(QString::number(info.dlss_sharpness, 'f', 2));

	m_ui->checkBox_dlss_fg_enable->setChecked(info.dlss_fg_enabled);

	m_ui->comboBox_dlss_fg_mode->clear();
	m_ui->comboBox_dlss_fg_mode->addItem(tr("2x Multiplier (DLSS 3 Frame Gen)"));
	m_ui->comboBox_dlss_fg_mode->addItem(tr("3x Multiplier (DLSS 4 Multi-Frame Gen)"));
	m_ui->comboBox_dlss_fg_mode->addItem(tr("4x Multiplier (DLSS 4 Multi-Frame Gen)"));
	m_ui->comboBox_dlss_fg_mode->addItem(tr("Dynamic Multiplier (Adaptive MFG)"));
	int fg_mode_idx = 0;
	switch (info.dlss_fg_mode) {
	case Configuration::DLSSGMode::FG_2X:   fg_mode_idx = 0; break;
	case Configuration::DLSSGMode::MFG_3X:  fg_mode_idx = 1; break;
	case Configuration::DLSSGMode::MFG_4X:  fg_mode_idx = 2; break;
	case Configuration::DLSSGMode::Dynamic: fg_mode_idx = 3; break;
	default:                                fg_mode_idx = 0; break;
	}
	m_ui->comboBox_dlss_fg_mode->setCurrentIndex(fg_mode_idx);

	m_ui->comboBox_reflex_mode->clear();
	m_ui->comboBox_reflex_mode->addItem(tr("On (Recommended)"));
	m_ui->comboBox_reflex_mode->addItem(tr("On + Boost"));
	m_ui->comboBox_reflex_mode->addItem(tr("Off"));
	int reflex_idx = 0;
	switch (info.reflex_mode) {
	case Configuration::ReflexMode::On:    reflex_idx = 0; break;
	case Configuration::ReflexMode::Boost: reflex_idx = 1; break;
	case Configuration::ReflexMode::Off:   reflex_idx = 2; break;
	default:                               reflex_idx = 0; break;
	}
	m_ui->comboBox_reflex_mode->setCurrentIndex(reflex_idx);

	m_ui->checkBox_dlss_diagnostics->setChecked(info.dlss_diagnostics);
	m_ui->checkBox_dlss_enable->setChecked(info.dlss_enabled);

	UpdateDLSSUI();
}

void ConfigurationEditDialog::UpdateDLSSUI() {
	int  gpu_sel           = m_ui->comboBox_gpu->currentIndex();
	bool gpu_supported     = m_has_nvidia_rtx;
	bool fg_gpu_supported  = m_has_nvidia_fg;
	bool mfg_gpu_supported = m_has_nvidia_mfg;
	if (gpu_sel > 0) {
		QString current_gpu_name = m_ui->comboBox_gpu->currentText();
		gpu_supported            = current_gpu_name.contains("NVIDIA", Qt::CaseInsensitive) &&
		                           current_gpu_name.contains("RTX", Qt::CaseInsensitive);
		fg_gpu_supported         = gpu_supported &&
		                           (current_gpu_name.contains("RTX 40", Qt::CaseInsensitive) ||
		                            current_gpu_name.contains("RTX 50", Qt::CaseInsensitive) ||
		                            current_gpu_name.contains("Ada", Qt::CaseInsensitive) ||
		                            current_gpu_name.contains("Blackwell", Qt::CaseInsensitive));
		mfg_gpu_supported        = gpu_supported &&
		                           (current_gpu_name.contains("RTX 50", Qt::CaseInsensitive) ||
		                            current_gpu_name.contains("Blackwell", Qt::CaseInsensitive));
	}

	bool dlss_supported = gpu_supported;
	bool fg_supported   = dlss_supported && fg_gpu_supported;

	if (!dlss_supported) {
		m_ui->checkBox_dlss_enable->setEnabled(false);
		m_ui->checkBox_dlss_enable->setChecked(false);
		if (!gpu_supported) {
			m_ui->checkBox_dlss_enable->setToolTip(
			    tr("NVIDIA DLSS requires a GeForce RTX GPU"));
			m_ui->label_dlss_status->setText(
			    tr("<font color='#f44336'><b>Unsupported GPU</b> (RTX required)</font>"));
		} else {
			m_ui->checkBox_dlss_enable->setToolTip(
			    tr("sl.interposer.dll or nvngx_dlss.dll not found in search paths"));
			m_ui->label_dlss_status->setText(
			    tr("<font color='#f44336'><b>Unsupported</b> (Streamline DLLs missing)</font>"));
		}
		m_ui->slider_dlss_sharpness->setEnabled(false);
		m_ui->checkBox_dlss_fg_enable->setEnabled(false);
		m_ui->checkBox_dlss_fg_enable->setChecked(false);
		m_ui->comboBox_dlss_fg_mode->setEnabled(false);
		m_ui->comboBox_reflex_mode->setEnabled(false);
		m_ui->checkBox_dlss_diagnostics->setEnabled(false);
	} else {
		m_ui->checkBox_dlss_enable->setEnabled(true);
		bool enabled = m_ui->checkBox_dlss_enable->isChecked();
		m_ui->slider_dlss_sharpness->setEnabled(enabled);
		m_ui->checkBox_dlss_diagnostics->setEnabled(enabled);

		// Frame Generation controls
		if (!fg_supported) {
			m_ui->checkBox_dlss_fg_enable->setEnabled(false);
			m_ui->checkBox_dlss_fg_enable->setChecked(false);
			m_ui->comboBox_dlss_fg_mode->setEnabled(false);
			if (!fg_gpu_supported) {
				m_ui->checkBox_dlss_fg_enable->setToolTip(
				    tr("Requires GeForce RTX 40 or 50 series GPU"));
			} else {
				m_ui->checkBox_dlss_fg_enable->setToolTip(
				    tr("sl.dlss_g.dll not found in search paths"));
			}
		} else {
			m_ui->checkBox_dlss_fg_enable->setEnabled(enabled);
			bool fg_enabled = enabled && m_ui->checkBox_dlss_fg_enable->isChecked();
			m_ui->comboBox_dlss_fg_mode->setEnabled(fg_enabled);
		}

		m_ui->comboBox_reflex_mode->setEnabled(enabled);

		if (m_ui->checkBox_dlss_fg_enable->isChecked() && m_ui->comboBox_reflex_mode->currentIndex() == 2) {
			m_ui->comboBox_reflex_mode->setCurrentIndex(0); // Reflex required for FG
		}

		int fg_mode_idx = m_ui->comboBox_dlss_fg_mode->currentIndex();
		if (enabled) {
			if (fg_supported && m_ui->checkBox_dlss_fg_enable->isChecked()) {
				QString fg_label;
				if (fg_mode_idx == 1) {
					fg_label = tr("MFG (3x)");
				} else if (fg_mode_idx == 2) {
					fg_label = tr("MFG (4x)");
				} else if (fg_mode_idx == 3) {
					fg_label = tr("Dynamic MFG");
				} else {
					fg_label = tr("FG (2x)");
				}
				m_ui->label_dlss_status->setText(
				    tr("<font color='#4caf50'><b>DLSS Active + %1</b></font>").arg(fg_label));
			} else {
				m_ui->label_dlss_status->setText(
				    tr("<font color='#4caf50'><b>DLSS Active</b></font>"));
			}
		} else {
			m_ui->label_dlss_status->setText(
			    tr("<font color='#2196f3'><b>DLSS Available</b> (Disabled)</font>"));
		}
	}

	int fg_mode_idx = m_ui->comboBox_dlss_fg_mode->currentIndex();
	QString fg_desc;
	switch (fg_mode_idx) {
	case 0:
		fg_desc = tr("Generates 1 frame per real frame (doubles FPS). Supported on RTX 40 & 50 series.");
		break;
	case 1:
		fg_desc = mfg_gpu_supported
		              ? tr("Generates 2 frames per real frame (triples FPS). Fully supported on your RTX 50 GPU.")
		              : tr("Generates 2 frames per real frame (triples FPS). Requires RTX 50 series (Blackwell).");
		break;
	case 2:
		fg_desc = mfg_gpu_supported
		              ? tr("Generates 3 frames per real frame (quadruples FPS). Fully supported on your RTX 50 GPU.")
		              : tr("Generates 3 frames per real frame (quadruples FPS). Requires RTX 50 series (Blackwell).");
		break;
	case 3:
	default:
		fg_desc = mfg_gpu_supported
		              ? tr("Dynamically adapts generated frames between 2x and 4x to match refresh rate. Supported on your RTX 50 GPU.")
		              : tr("Dynamically adapts generated frames between 2x and 4x based on display refresh rate. Requires RTX 50 series (Blackwell).");
		break;
	}
	m_ui->label_dlss_fg_desc->setText(fg_desc);

	if (m_ui->checkBox_dlss_enable->isChecked()) {
		m_ui->label_dlss_render_res->setText(
		    tr("Internal render resolution is selected in Game Patches (.json)"));
	} else {
		m_ui->label_dlss_render_res->setText(
		    tr("Native render: %1").arg(m_ui->comboBox_screen_resolution->currentText()));
	}
}

void ConfigurationEditDialog::InitGameDirectories() {
	m_game_dirs_group = new QGroupBox(tr("Game folders"), this);

	auto* group_layout = new QVBoxLayout(m_game_dirs_group);
	group_layout->setContentsMargins(8, 8, 8, 8);
	group_layout->setSpacing(6);

	m_game_dirs_list = new QListWidget(m_game_dirs_group);
	m_game_dirs_list->setMinimumHeight(120);
	m_game_dirs_list->setSelectionMode(QAbstractItemView::ExtendedSelection);
	group_layout->addWidget(m_game_dirs_list);

	auto* button_layout = new QHBoxLayout;
	button_layout->setContentsMargins(0, 0, 0, 0);
	button_layout->setSpacing(4);

	auto* add_button = new QToolButton(m_game_dirs_group);
	add_button->setIcon(style()->standardIcon(QStyle::SP_DirOpenIcon));
	add_button->setToolTip(tr("Add game folder"));
	add_button->setAutoRaise(true);
	button_layout->addWidget(add_button);

	m_remove_game_dir_button = new QToolButton(m_game_dirs_group);
	m_remove_game_dir_button->setIcon(style()->standardIcon(QStyle::SP_DialogDiscardButton));
	m_remove_game_dir_button->setToolTip(tr("Remove selected game folders"));
	m_remove_game_dir_button->setAutoRaise(true);
	button_layout->addWidget(m_remove_game_dir_button);

	button_layout->addStretch(1);
	group_layout->addLayout(button_layout);

	m_ui->gridLayout->addWidget(m_game_dirs_group, 1, 0, 1, 1);
	m_game_dirs_group->setVisible(false);

	connect(add_button, &QToolButton::clicked, this, &ConfigurationEditDialog::add_game_directory);
	connect(m_remove_game_dir_button, &QToolButton::clicked, this,
	        &ConfigurationEditDialog::remove_selected_game_directories);
	connect(m_game_dirs_list, &QListWidget::itemSelectionChanged, this,
	        &ConfigurationEditDialog::update_game_directory_buttons);

	update_game_directory_buttons();
}

void ConfigurationEditDialog::SetGlobalSettings(const QStringList& dirs) {
	m_global_settings = true;
	m_game_dirs_list->clear();
	m_game_dirs_group->setMinimumWidth(GLOBAL_SETTINGS_GAME_DIRS_MIN_WIDTH);

	for (const auto& dir: dirs) {
		AddGameDirectoryItem(dir);
	}

	m_game_dirs_group->setVisible(true);
	m_ui->controller_group->setVisible(true);
	SetAudioMixVisible(*m_ui, true);
	update_game_directory_buttons();
	layout()->activate();
	resize(size().expandedTo(minimumSizeHint()));
}

QStringList ConfigurationEditDialog::GetGameDirectories() const {
	QStringList dirs;
	if (!m_global_settings) {
		return dirs;
	}

	for (int index = 0; index < m_game_dirs_list->count(); index++) {
		auto* item = m_game_dirs_list->item(index);
		dirs.append(item->text());
	}

	return dirs;
}

void ConfigurationEditDialog::AddGameDirectoryItem(const QString& dir) {
	const auto normalized = NormalizeGameDirectory(dir);
	const auto key        = GameDirectoryKey(normalized);
	if (normalized.isEmpty() || key.isEmpty()) {
		return;
	}

	for (int index = 0; index < m_game_dirs_list->count(); index++) {
		auto* item = m_game_dirs_list->item(index);
		if (GameDirectoryKey(item->text()) == key) {
			return;
		}
	}

	auto* item = new QListWidgetItem(normalized, m_game_dirs_list);
	item->setToolTip(normalized);
}

void ConfigurationEditDialog::moveEvent(QMoveEvent* event) {
	QDialog::moveEvent(event);
	g_last_geometry = saveGeometry();
}

void ConfigurationEditDialog::resizeEvent(QResizeEvent* event) {
	QDialog::resizeEvent(event);
	g_last_geometry = saveGeometry();
}

static void UpdateInfo(Configuration& info, Ui::ConfigurationEditDialog& ui, bool global_settings) {
	info.user_name = ui.lineEdit_user_name->text().trimmed();
	info.user_id   = ui.spinBox_user_id->value();
	info.audio_input_device = ui.comboBox_audio_input_device->currentData().toString();
	if (global_settings) {
		info.controller.color = ui.button_controller_color->property("controllerColor").toString();
		info.controller.vibration_intensity = ui.slider_controller_vibration->value();
		info.controller.speaker_volume      = ui.slider_controller_volume->value();
		for (const auto& mix: AudioMixSliders(ui)) {
			info.audio_mix.*mix.field = mix.slider->value();
		}
		info.gpu_occlusion_accurate = ui.checkBox_gpu_occlusion->isChecked();
	}
	info.screen_resolution =
	    TextToEnum<Configuration::Resolution>(ui.comboBox_screen_resolution->currentText());
	info.present_mode =
	    TextToEnum<Configuration::PresentMode>(ui.comboBox_present_mode->currentText());
	info.gpu_index                 = ui.comboBox_gpu->currentIndex() - 1;
	info.fullscreen_enabled        = ui.checkBox_fullscreen->isChecked();
	info.hide_cursor_enabled       = ui.checkBox_hide_cursor->isChecked();
	info.readback_linear_images    = ui.checkBox_readback->isChecked();
	info.tessellation_enabled      = ui.checkBox_tessellation->isChecked();
	info.trophy_enabled            = ui.checkBox_trophy_notifications->isChecked();
	info.vblank_frequency          = ui.spinBox_vblank_frequency->value();
	info.console_language          = ui.comboBox_console_language->currentIndex();
	info.vulkan_validation_enabled = ui.checkBox_vulkan_validation->isChecked();
	info.shader_validation_enabled = ui.checkBox_shader_validation->isChecked();
	info.renderdoc_enabled         = ui.checkBox_renderdoc_capture->isChecked();
	info.amd_cpu_enabled           = ui.checkBox_amd_cpu->isChecked();
#if defined(_WIN32)
	info.red_zone_protection_enabled = ui.checkBox_red_zone_protection->isChecked();
#endif
	info.shader_optimization_type = TextToEnum<Configuration::ShaderOptimizationType>(
	    ui.comboBox_shader_optimization_type->currentText());
	info.shader_log_direction = TextToEnum<Configuration::LogDirection>(
	    ui.comboBox_shader_log_direction->currentText());
	info.shader_log_folder           = ui.lineEdit_shader_log_folder->text();
	info.command_buffer_dump_enabled = ui.checkBox_cmd_dump->isChecked();
	info.command_buffer_dump_folder  = ui.lineEdit_cmd_dump_folder->text();
	info.printf_direction =
	    TextToEnum<Configuration::LogDirection>(ui.comboBox_printf_direction->currentText());
	info.printf_output_file = ui.lineEdit_printf_file->text();
	info.profiler_enabled = ui.checkBox_profiler->isChecked();
	info.dlss_enabled     = ui.checkBox_dlss_enable->isChecked();
	info.dlss_mode        = Configuration::DLSSMode::Quality;
	info.dlss_sharpness   = ui.slider_dlss_sharpness->value() / 100.0f;
	info.dlss_preset      = Configuration::DLSSPreset::Default;
	info.dlss_diagnostics = ui.checkBox_dlss_diagnostics->isChecked();
	info.dlss_fg_enabled  = info.dlss_enabled && ui.checkBox_dlss_fg_enable->isChecked();
	switch (ui.comboBox_dlss_fg_mode->currentIndex()) {
	case 0:  info.dlss_fg_mode = Configuration::DLSSGMode::FG_2X; break;
	case 1:  info.dlss_fg_mode = Configuration::DLSSGMode::MFG_3X; break;
	case 2:  info.dlss_fg_mode = Configuration::DLSSGMode::MFG_4X; break;
	case 3:  info.dlss_fg_mode = Configuration::DLSSGMode::Dynamic; break;
	default: info.dlss_fg_mode = Configuration::DLSSGMode::FG_2X; break;
	}
	switch (ui.comboBox_reflex_mode->currentIndex()) {
	case 0:  info.reflex_mode = Configuration::ReflexMode::On; break;
	case 1:  info.reflex_mode = Configuration::ReflexMode::Boost; break;
	case 2:  info.reflex_mode = Configuration::ReflexMode::Off; break;
	default: info.reflex_mode = Configuration::ReflexMode::On; break;
	}
}

void ConfigurationEditDialog::save() {
	if (MandatoryLineEdit::FindEmpty(this)) {
		QMessageBox::critical(this, tr("Save failed"), tr("Please fill all mandatory fields"));
		return;
	}

	const auto user_name = m_ui->lineEdit_user_name->text().trimmed();
	if (user_name.isEmpty() || user_name.toUtf8().size() > Config::MAX_USER_NAME_LENGTH) {
		QMessageBox::critical(this, tr("Save failed"),
		                      tr("User name must contain 1-16 UTF-8 bytes"));
		return;
	}
	m_ui->lineEdit_user_name->setText(user_name);
	if (!Config::IsConfiguredUserIdValid(m_ui->spinBox_user_id->value())) {
		QMessageBox::critical(this, tr("Save failed"),
		                      tr("User ID cannot be 254 (everyone) or 255 (system)"));
		return;
	}

	UpdateInfo(m_info, *m_ui, m_global_settings);
#if defined(_WIN32)
	if (m_directstorage_checkbox != nullptr) {
		m_info.directstorage_enabled = m_directstorage_checkbox->isChecked();
	}
#endif

	emit accept();
}

void ConfigurationEditDialog::clear() {
	Configuration default_info;
	Init(default_info);

	if (m_global_settings) {
		emit PreviewControllerColor({});
		m_game_dirs_list->clear();
		update_game_directory_buttons();
	}
}

void ConfigurationEditDialog::add_game_directory() {
	QString start_dir = QDir::homePath();
	if (m_game_dirs_list->count() > 0) {
		start_dir = m_game_dirs_list->item(m_game_dirs_list->count() - 1)->text();
	}

#if defined(__APPLE__)
	// Use the native macOS picker to browse mounted volumes.
	const auto dir = QFileDialog::getExistingDirectory(this, tr("Select game folder"), start_dir);
	if (dir.isEmpty()) {
		return;
	}
	AddGameDirectoryItem(dir);
#else
	QFileDialog dialog(this, tr("Select game folders"), start_dir);
	dialog.setFileMode(QFileDialog::Directory);
	dialog.setOption(QFileDialog::ShowDirsOnly, true);
	dialog.setOption(QFileDialog::DontUseNativeDialog, true);
	dialog.setLabelText(QFileDialog::Accept, tr("Add"));

	auto* list_view = dialog.findChild<QListView*>(QStringLiteral("listView"));
	if (list_view != nullptr) {
		list_view->setSelectionMode(QAbstractItemView::ExtendedSelection);
	}
	auto* tree_view = dialog.findChild<QTreeView*>(QStringLiteral("treeView"));
	if (tree_view != nullptr) {
		tree_view->setSelectionMode(QAbstractItemView::ExtendedSelection);
	}

	if (dialog.exec() != QDialog::Accepted) {
		return;
	}

	for (const auto& dir: dialog.selectedFiles()) {
		AddGameDirectoryItem(dir);
	}
#endif

	update_game_directory_buttons();
}

void ConfigurationEditDialog::remove_selected_game_directories() {
	qDeleteAll(m_game_dirs_list->selectedItems());
	update_game_directory_buttons();
}

void ConfigurationEditDialog::update_game_directory_buttons() {
	m_remove_game_dir_button->setEnabled(!m_game_dirs_list->selectedItems().isEmpty());
}
