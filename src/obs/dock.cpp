// Qt dock "AV-Sync": reference clock, phone link, live measurements, A/V offset, sessions, settings.
#include "plugin.hpp"
#include "qrcodegen.hpp"

#include <obs-frontend-api.h>

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFontDatabase>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QTableWidget>
#include <QTimeEdit>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace avs {

namespace {

QString T(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

QString ms(double v, int decimals = 1)
{
	if (!std::isfinite(v))
		return QStringLiteral("–");
	return QLocale().toString(v, 'f', decimals);
}

QString signedMs(double v, int decimals = 1)
{
	if (!std::isfinite(v))
		return QStringLiteral("–");
	QString s = QLocale().toString(std::fabs(v), 'f', decimals);
	return (v > 0 ? QStringLiteral("+") : v < 0 ? QStringLiteral("−") : QStringLiteral("±")) + s;
}

QPixmap makeQr(const QString &text, int targetPx)
{
	using qrcodegen::QrCode;
	QrCode qr = QrCode::encodeText(text.toUtf8().constData(), QrCode::Ecc::MEDIUM);
	const int border = 2;
	const int n = qr.getSize() + 2 * border;
	QImage img(n, n, QImage::Format_RGB32);
	img.fill(Qt::white);
	for (int y = 0; y < qr.getSize(); y++)
		for (int x = 0; x < qr.getSize(); x++)
			if (qr.getModule(x, y))
				img.setPixel(x + border, y + border, qRgb(0, 0, 0));
	const int scale = std::max(1, targetPx / n);
	return QPixmap::fromImage(img.scaled(n * scale, n * scale, Qt::IgnoreAspectRatio, Qt::FastTransformation));
}

// ---------------------------------------------------------------- latency graph

class LatencyGraph : public QWidget {
public:
	struct Series {
		QString name;
		QColor color;
		std::vector<std::pair<int64_t, double>> points; // seq, ms
	};

	explicit LatencyGraph(QWidget *parent = nullptr) : QWidget(parent)
	{
		setMinimumHeight(130);
		setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	}

	void setSeries(std::vector<Series> s)
	{
		series_ = std::move(s);
		update();
	}

protected:
	void paintEvent(QPaintEvent *) override
	{
		QPainter p(this);
		p.setRenderHint(QPainter::Antialiasing);
		const QRectF area = rect().adjusted(42, 8, -8, -18);
		p.fillRect(rect(), palette().base());

		int64_t s0 = INT64_MAX, s1 = INT64_MIN;
		double lo = 1e18, hi = -1e18;
		for (const auto &s : series_)
			for (const auto &pt : s.points) {
				s0 = std::min(s0, pt.first);
				s1 = std::max(s1, pt.first);
				lo = std::min(lo, pt.second);
				hi = std::max(hi, pt.second);
			}
		p.setPen(palette().color(QPalette::Mid));
		p.drawRect(area);
		if (s0 > s1) {
			p.setPen(palette().color(QPalette::PlaceholderText));
			p.drawText(area, Qt::AlignCenter, T("Dock.Graph.Empty"));
			return;
		}
		if (hi - lo < 10.0) {
			double mid = (hi + lo) / 2;
			lo = mid - 5;
			hi = mid + 5;
		}
		const double pad = (hi - lo) * 0.1;
		lo -= pad;
		hi += pad;
		if (s1 == s0)
			s1 = s0 + 1;

		auto X = [&](int64_t seq) { return area.left() + (double)(seq - s0) / (double)(s1 - s0) * area.width(); };
		auto Y = [&](double v) { return area.bottom() - (v - lo) / (hi - lo) * area.height(); };

		// grid lines
		const double range = hi - lo;
		const double steps[] = {1, 2, 5, 10, 20, 25, 50, 100, 200, 250, 500, 1000, 2000, 5000};
		double step = steps[0];
		for (double st : steps) {
			step = st;
			if (range / st <= 5)
				break;
		}
		QFont f = font();
		f.setPointSizeF(std::max(6.0, f.pointSizeF() * 0.8));
		p.setFont(f);
		for (double v = std::ceil(lo / step) * step; v <= hi; v += step) {
			const double y = Y(v);
			p.setPen(QPen(palette().color(QPalette::Mid), 1, v == 0 ? Qt::SolidLine : Qt::DotLine));
			p.drawLine(QPointF(area.left(), y), QPointF(area.right(), y));
			p.setPen(palette().color(QPalette::Text));
			p.drawText(QRectF(0, y - 8, area.left() - 4, 16), Qt::AlignRight | Qt::AlignVCenter, ms(v, 0));
		}

		// series
		int legendX = (int)area.left() + 4;
		for (const auto &s : series_) {
			if (s.points.empty())
				continue;
			QPainterPath path;
			bool first = true;
			int64_t prev = 0;
			for (const auto &pt : s.points) {
				QPointF q(X(pt.first), Y(pt.second));
				if (first || pt.first - prev > 3)
					path.moveTo(q);
				else
					path.lineTo(q);
				first = false;
				prev = pt.first;
			}
			p.setPen(QPen(s.color, 1.6));
			p.drawPath(path);
			p.setBrush(s.color);
			for (const auto &pt : s.points)
				p.drawEllipse(QPointF(X(pt.first), Y(pt.second)), 1.8, 1.8);
			p.setBrush(Qt::NoBrush);
			p.setPen(s.color);
			const QString label = s.name;
			p.drawText(QPoint(legendX, (int)area.bottom() + 14), label);
			legendX += p.fontMetrics().horizontalAdvance(label) + 14;
		}
	}

private:
	std::vector<Series> series_;
};

// ---------------------------------------------------------------- dock widget

class AvSyncDock : public QWidget {
public:
	explicit AvSyncDock(QWidget *parent = nullptr);

private:
	QGroupBox *buildClock();
	QGroupBox *buildPhone();
	QGroupBox *buildTable();
	QGroupBox *buildOffset();
	QGroupBox *buildSession();
	QWidget *buildSettings();

	void refresh();
	void refreshCombos(const std::vector<ProbeSnapshot> &snap);
	void refreshTable(const std::vector<ProbeSnapshot> &snap);
	void refreshOffset(const std::vector<ProbeSnapshot> &snap);
	void refreshSession();
	void loadSettingsIntoUi();
	void applySettings();
	void applyOffset();
	void startSession(bool now);

	QTimer timer_;
	QWidget *content_ = nullptr;

	QLabel *clockLabel_ = nullptr;
	QLabel *clockStatus_ = nullptr;

	QLabel *qrLabel_ = nullptr;
	QLabel *qrLanLabel_ = nullptr;
	QWidget *qrColumn_ = nullptr;
	QWidget *qrLanColumn_ = nullptr;
	QString qrLanText_;
	QLabel *urlLabel_ = nullptr;
	QLabel *phonesLabel_ = nullptr;
	QString qrText_;

	QTableWidget *table_ = nullptr;

	QComboBox *videoCombo_ = nullptr;
	QComboBox *audioCombo_ = nullptr;
	QLabel *offsetLabel_ = nullptr;
	QLabel *offsetHint_ = nullptr;
	QPushButton *applyButton_ = nullptr;
	LatencyGraph *graph_ = nullptr;
	std::vector<int> comboVideoIds_, comboAudioIds_;

	QTimeEdit *startEdit_ = nullptr;
	QSpinBox *durationSpin_ = nullptr;
	QLineEdit *labelEdit_ = nullptr;
	QLabel *sessionLabel_ = nullptr;
	QPushButton *cancelButton_ = nullptr;

	QSpinBox *portSpin_ = nullptr;
	QCheckBox *serverCheck_ = nullptr;
	QCheckBox *ntpCheck_ = nullptr;
	QLineEdit *ntpEdit_ = nullptr;
	QCheckBox *progVideoCheck_ = nullptr;
	QCheckBox *progAudioCheck_ = nullptr;
	QSpinBox *trackSpin_ = nullptr;
	QSpinBox *maxLatSpin_ = nullptr;
	QSpinBox *windowSpin_ = nullptr;
	QLineEdit *csvEdit_ = nullptr;
	QCheckBox *csvGermanCheck_ = nullptr;
	QLineEdit *publicUrlEdit_ = nullptr;
};

AvSyncDock::AvSyncDock(QWidget *parent) : QWidget(parent)
{
	auto *outer = new QVBoxLayout(this);
	outer->setContentsMargins(0, 0, 0, 0);
	auto *scroll = new QScrollArea(this);
	scroll->setWidgetResizable(true);
	scroll->setFrameShape(QFrame::NoFrame);
	outer->addWidget(scroll);

	auto *content = new QWidget(scroll);
	content_ = content;
	auto *lay = new QVBoxLayout(content);
	lay->setContentsMargins(6, 6, 6, 6);
	lay->setSpacing(8);
	lay->addWidget(buildClock());
	lay->addWidget(buildPhone());
	lay->addWidget(buildTable());
	lay->addWidget(buildOffset());
	lay->addWidget(buildSession());
	lay->addWidget(buildSettings());
	lay->addStretch(1);
	scroll->setWidget(content);

	loadSettingsIntoUi();
	connect(&timer_, &QTimer::timeout, this, [this] { refresh(); });
	timer_.start(200);
	refresh();
}

QGroupBox *AvSyncDock::buildClock()
{
	auto *box = new QGroupBox(T("Dock.Clock"), this);
	auto *lay = new QVBoxLayout(box);
	clockLabel_ = new QLabel(QStringLiteral("--:--:--.---"), box);
	// a widget style sheet wins over the OBS theme (setFont() alone does not)
	clockLabel_->setStyleSheet(QStringLiteral(
		"font-family: 'Consolas', 'Cascadia Mono', 'Menlo', monospace; font-size: 22pt; font-weight: bold;"));
	clockLabel_->setAlignment(Qt::AlignCenter);
	clockStatus_ = new QLabel(box);
	clockStatus_->setWordWrap(true);
	clockStatus_->setAlignment(Qt::AlignCenter);
	lay->addWidget(clockLabel_);
	lay->addWidget(clockStatus_);
	return box;
}

QGroupBox *AvSyncDock::buildPhone()
{
	auto *box = new QGroupBox(T("Dock.Phone"), this);
	auto *lay = new QVBoxLayout(box);
	auto *qrRow = new QHBoxLayout();
	auto addQr = [&](QLabel *&qr, QWidget *&column, const QString &caption) {
		column = new QWidget(box);
		auto *col = new QVBoxLayout(column);
		col->setContentsMargins(0, 0, 0, 0);
		qr = new QLabel(column);
		qr->setFixedSize(116, 116);
		qr->setAlignment(Qt::AlignCenter);
		auto *cap = new QLabel(caption, column);
		cap->setAlignment(Qt::AlignCenter);
		col->addWidget(qr, 0, Qt::AlignHCenter);
		col->addWidget(cap);
		qrRow->addWidget(column);
	};
	addQr(qrLabel_, qrColumn_, T("Dock.Phone.QrOnline"));
	addQr(qrLanLabel_, qrLanColumn_, T("Dock.Phone.QrLan"));
	qrRow->addStretch(1);
	lay->addLayout(qrRow);

	urlLabel_ = new QLabel(box);
	urlLabel_->setWordWrap(true);
	urlLabel_->setTextFormat(Qt::RichText);
	urlLabel_->setOpenExternalLinks(true);
	urlLabel_->setTextInteractionFlags(Qt::TextBrowserInteraction);
	phonesLabel_ = new QLabel(box);
	phonesLabel_->setWordWrap(true);
	lay->addWidget(urlLabel_);
	lay->addWidget(phonesLabel_);
	return box;
}

QGroupBox *AvSyncDock::buildTable()
{
	auto *box = new QGroupBox(T("Dock.Measurements"), this);
	auto *lay = new QVBoxLayout(box);
	table_ = new QTableWidget(0, 6, box);
	table_->setHorizontalHeaderLabels({T("Dock.Col.Probe"), T("Dock.Col.Last"), T("Dock.Col.Median"),
					   T("Dock.Col.Jitter"), T("Dock.Col.Count"), T("Dock.Col.Status")});
	table_->verticalHeader()->setVisible(false);
	table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
	table_->setSelectionMode(QAbstractItemView::NoSelection);
	table_->setFocusPolicy(Qt::NoFocus);
	table_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
	table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
	table_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	table_->setSizeAdjustPolicy(QAbstractScrollArea::AdjustToContents);
	auto *hint = new QLabel(T("Dock.Measurements.Hint"), box);
	hint->setWordWrap(true);
	hint->setStyleSheet(QStringLiteral("color: palette(placeholder-text);"));
	auto *reset = new QPushButton(T("Dock.Reset"), box);
	connect(reset, &QPushButton::clicked, this, [] { Plugin::instance().engine().resetStats(); });
	lay->addWidget(table_);
	lay->addWidget(hint);
	lay->addWidget(reset, 0, Qt::AlignRight);
	return box;
}

QGroupBox *AvSyncDock::buildOffset()
{
	auto *box = new QGroupBox(T("Dock.Offset"), this);
	auto *lay = new QVBoxLayout(box);
	auto *form = new QFormLayout();
	videoCombo_ = new QComboBox(box);
	audioCombo_ = new QComboBox(box);
	form->addRow(T("Dock.Offset.Video"), videoCombo_);
	form->addRow(T("Dock.Offset.Audio"), audioCombo_);
	lay->addLayout(form);

	auto pairChanged = [this] {
		const int vi = videoCombo_->currentIndex(), ai = audioCombo_->currentIndex();
		const int v = vi >= 0 && vi < (int)comboVideoIds_.size() ? comboVideoIds_[(size_t)vi] : -1;
		const int a = ai >= 0 && ai < (int)comboAudioIds_.size() ? comboAudioIds_[(size_t)ai] : -1;
		Plugin::instance().setPair(v, a);
	};
	connect(videoCombo_, QOverload<int>::of(&QComboBox::activated), this, pairChanged);
	connect(audioCombo_, QOverload<int>::of(&QComboBox::activated), this, pairChanged);

	offsetLabel_ = new QLabel(QStringLiteral("–"), box);
	offsetLabel_->setStyleSheet(QStringLiteral("font-size: 18pt; font-weight: bold;"));
	offsetLabel_->setAlignment(Qt::AlignCenter);
	offsetHint_ = new QLabel(box);
	offsetHint_->setWordWrap(true);
	offsetHint_->setAlignment(Qt::AlignCenter);
	applyButton_ = new QPushButton(T("Dock.Offset.Apply"), box);
	connect(applyButton_, &QPushButton::clicked, this, [this] { applyOffset(); });
	graph_ = new LatencyGraph(box);

	lay->addWidget(offsetLabel_);
	lay->addWidget(offsetHint_);
	lay->addWidget(applyButton_);
	lay->addWidget(graph_);
	return box;
}

QGroupBox *AvSyncDock::buildSession()
{
	auto *box = new QGroupBox(T("Dock.Session"), this);
	auto *lay = new QVBoxLayout(box);
	auto *form = new QFormLayout();
	startEdit_ = new QTimeEdit(box);
	startEdit_->setDisplayFormat(QStringLiteral("HH:mm:ss"));
	startEdit_->setTime(QTime::currentTime().addSecs(60));
	durationSpin_ = new QSpinBox(box);
	durationSpin_->setRange(5, 24 * 3600);
	durationSpin_->setSuffix(QStringLiteral(" s"));
	labelEdit_ = new QLineEdit(box);
	labelEdit_->setPlaceholderText(T("Dock.Session.LabelPlaceholder"));
	form->addRow(T("Dock.Session.Start"), startEdit_);
	form->addRow(T("Dock.Session.Duration"), durationSpin_);
	form->addRow(T("Dock.Session.Label"), labelEdit_);
	lay->addLayout(form);

	auto *buttons = new QHBoxLayout();
	auto *plan = new QPushButton(T("Dock.Session.Schedule"), box);
	auto *now = new QPushButton(T("Dock.Session.Now"), box);
	cancelButton_ = new QPushButton(T("Dock.Session.Cancel"), box);
	auto *folder = new QToolButton(box);
	folder->setText(T("Dock.Session.OpenFolder"));
	connect(plan, &QPushButton::clicked, this, [this] { startSession(false); });
	connect(now, &QPushButton::clicked, this, [this] { startSession(true); });
	connect(cancelButton_, &QPushButton::clicked, this, [] { Plugin::instance().engine().cancelSession(); });
	connect(folder, &QToolButton::clicked, this, [] {
		const QString dir = QString::fromStdString(Plugin::instance().config().csvFolder);
		QDir().mkpath(dir);
		QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
	});
	buttons->addWidget(plan);
	buttons->addWidget(now);
	buttons->addWidget(cancelButton_);
	buttons->addWidget(folder);
	lay->addLayout(buttons);

	sessionLabel_ = new QLabel(box);
	sessionLabel_->setWordWrap(true);
	sessionLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
	lay->addWidget(sessionLabel_);
	return box;
}

QWidget *AvSyncDock::buildSettings()
{
	auto *wrap = new QWidget(this);
	auto *outer = new QVBoxLayout(wrap);
	outer->setContentsMargins(0, 0, 0, 0);
	auto *toggle = new QToolButton(wrap);
	toggle->setText(T("Dock.Settings"));
	toggle->setCheckable(true);
	toggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
	toggle->setArrowType(Qt::RightArrow);
	toggle->setStyleSheet(QStringLiteral("QToolButton { border: none; font-weight: bold; }"));
	auto *box = new QGroupBox(wrap);
	box->setVisible(false);
	connect(toggle, &QToolButton::toggled, this, [toggle, box](bool on) {
		box->setVisible(on);
		toggle->setArrowType(on ? Qt::DownArrow : Qt::RightArrow);
	});
	outer->addWidget(toggle);
	outer->addWidget(box);

	auto *form = new QFormLayout(box);
	serverCheck_ = new QCheckBox(T("Dock.Settings.Server"), box);
	portSpin_ = new QSpinBox(box);
	portSpin_->setRange(1024, 65535);
	ntpCheck_ = new QCheckBox(T("Dock.Settings.Ntp"), box);
	ntpEdit_ = new QLineEdit(box);
	progVideoCheck_ = new QCheckBox(T("Dock.Settings.ProgramVideo"), box);
	progAudioCheck_ = new QCheckBox(T("Dock.Settings.ProgramAudio"), box);
	trackSpin_ = new QSpinBox(box);
	trackSpin_->setRange(1, 6);
	maxLatSpin_ = new QSpinBox(box);
	maxLatSpin_->setRange(200, 9000);
	maxLatSpin_->setSingleStep(100);
	maxLatSpin_->setSuffix(QStringLiteral(" ms"));
	maxLatSpin_->setToolTip(T("Dock.Settings.MaxLatency.Tip"));
	windowSpin_ = new QSpinBox(box);
	windowSpin_->setRange(3, 600);
	csvEdit_ = new QLineEdit(box);
	auto *browse = new QToolButton(box);
	browse->setText(QStringLiteral("…"));
	connect(browse, &QToolButton::clicked, this, [this] {
		QString dir = QFileDialog::getExistingDirectory(this, T("Dock.Settings.CsvFolder"), csvEdit_->text());
		if (!dir.isEmpty())
			csvEdit_->setText(QDir::toNativeSeparators(dir));
	});
	auto *csvRow = new QHBoxLayout();
	csvRow->addWidget(csvEdit_, 1);
	csvRow->addWidget(browse);
	csvGermanCheck_ = new QCheckBox(T("Dock.Settings.CsvGerman"), box);
	publicUrlEdit_ = new QLineEdit(box);
	publicUrlEdit_->setPlaceholderText(QStringLiteral("https://<name>.github.io/obs-avsync/"));

	form->addRow(serverCheck_);
	form->addRow(T("Dock.Settings.Port"), portSpin_);
	form->addRow(T("Dock.Settings.PublicUrl"), publicUrlEdit_);
	form->addRow(ntpCheck_);
	form->addRow(T("Dock.Settings.NtpServers"), ntpEdit_);
	form->addRow(progVideoCheck_);
	form->addRow(progAudioCheck_);
	form->addRow(T("Dock.Settings.Track"), trackSpin_);
	form->addRow(T("Dock.Settings.MaxLatency"), maxLatSpin_);
	form->addRow(T("Dock.Settings.Window"), windowSpin_);
	form->addRow(T("Dock.Settings.CsvFolder"), csvRow);
	form->addRow(csvGermanCheck_);
	auto *apply = new QPushButton(T("Dock.Settings.Apply"), box);
	connect(apply, &QPushButton::clicked, this, [this] { applySettings(); });
	form->addRow(apply);
	return wrap;
}

void AvSyncDock::loadSettingsIntoUi()
{
	Config c = Plugin::instance().config();
	serverCheck_->setChecked(c.serverEnabled);
	portSpin_->setValue(c.port);
	ntpCheck_->setChecked(c.ntpEnabled);
	ntpEdit_->setText(QString::fromStdString(c.ntpServers));
	progVideoCheck_->setChecked(c.programVideo);
	progAudioCheck_->setChecked(c.programAudio);
	trackSpin_->setValue(c.programTrack);
	maxLatSpin_->setValue((int)c.maxLatencyMs);
	windowSpin_->setValue(c.liveWindow);
	csvEdit_->setText(QString::fromStdString(c.csvFolder));
	csvGermanCheck_->setChecked(c.csvGerman);
	publicUrlEdit_->setText(QString::fromStdString(c.publicPageUrl));
	durationSpin_->setValue(c.sessionDurationS);
}

void AvSyncDock::applySettings()
{
	Config c = Plugin::instance().config();
	c.serverEnabled = serverCheck_->isChecked();
	c.port = portSpin_->value();
	c.ntpEnabled = ntpCheck_->isChecked();
	c.ntpServers = ntpEdit_->text().toStdString();
	c.programVideo = progVideoCheck_->isChecked();
	c.programAudio = progAudioCheck_->isChecked();
	c.programTrack = trackSpin_->value();
	c.maxLatencyMs = maxLatSpin_->value();
	c.liveWindow = windowSpin_->value();
	c.csvFolder = csvEdit_->text().toStdString();
	c.csvGerman = csvGermanCheck_->isChecked();
	c.publicPageUrl = publicUrlEdit_->text().trimmed().toStdString();
	Plugin::instance().applyConfig(c);
	qrText_ = QStringLiteral("-");
	qrLanText_ = QStringLiteral("-");
}

void AvSyncDock::startSession(bool now)
{
	Plugin &p = Plugin::instance();
	double start = 0.0; // next full second
	if (!now) {
		const double utcNow = p.nowUtcMs();
		const QDateTime nowLocal = QDateTime::fromMSecsSinceEpoch((qint64)utcNow);
		QDateTime dt(nowLocal.date(), startEdit_->time());
		if (dt.toMSecsSinceEpoch() <= (qint64)utcNow)
			dt = dt.addDays(1);
		start = (double)dt.toMSecsSinceEpoch();
	}
	p.startSession(start, durationSpin_->value() * 1000.0, labelEdit_->text().toStdString());
	Config c = p.config();
	if (c.sessionDurationS != durationSpin_->value()) {
		c.sessionDurationS = durationSpin_->value();
		p.applyConfig(c);
	}
}

void AvSyncDock::applyOffset()
{
	Plugin &p = Plugin::instance();
	int v, a;
	p.resolvedPair(v, a);
	auto snap = p.engine().snapshot(0);
	const ProbeSnapshot *audio = nullptr;
	for (const auto &s : snap)
		if (s.id == a)
			audio = &s;
	if (!audio || audio->sourceUuid.empty())
		return;
	PairStats ps = p.engine().pair(v, a, (size_t)p.config().liveWindow);
	if (ps.offset.count < 3)
		return;

	obs_source_t *src = obs_get_source_by_uuid(audio->sourceUuid.c_str());
	if (!src)
		return;
	const int64_t current = obs_source_get_sync_offset(src);
	// offset = audio latency - video latency -> shift the audio by -offset
	const int64_t proposed = current - (int64_t)std::llround(ps.offset.median * 1e6);
	const QString text = T("Dock.Offset.Confirm")
				     .arg(QString::fromStdString(audio->name))
				     .arg(ms(current / 1e6, 0))
				     .arg(ms(proposed / 1e6, 0));
	if (QMessageBox::question(this, T("Dock.Offset.Apply"), text) == QMessageBox::Yes) {
		obs_source_set_sync_offset(src, proposed);
		p.engine().resetStats();
		blog(LOG_INFO, "[obs-avsync] sync offset of '%s' set to %.0f ms", audio->name.c_str(), proposed / 1e6);
	}
	obs_source_release(src);
}

void AvSyncDock::refresh()
{
	if (!isVisible())
		return;
	Plugin &p = Plugin::instance();

	// test hook: AVSYNC_DOCK_SNAPSHOT=<file.png> saves a screenshot of the dock every 5 s
	static const char *snapshotPath = getenv("AVSYNC_DOCK_SNAPSHOT");
	static int snapshotCounter = 0;
	if (snapshotPath && ++snapshotCounter % 25 == 0)
		content_->grab().save(QString::fromLocal8Bit(snapshotPath));

	// clock
	const double utc = p.nowUtcMs();
	clockLabel_->setText(QDateTime::fromMSecsSinceEpoch((qint64)utc).toString(QStringLiteral("HH:mm:ss.zzz")));
	ClockStatus cs = p.clock().status();
	if (cs.ntpSynced)
		clockStatus_->setText(T("Dock.Clock.Ntp").arg(QString::fromStdString(cs.server), ms(cs.errorMs),
							       signedMs(cs.systemOffsetMs, 0)));
	else
		clockStatus_->setText(T("Dock.Clock.System").arg(QString::fromStdString(cs.lastError)));
	clockStatus_->setStyleSheet(cs.ntpSynced ? QString() : QStringLiteral("color: #e0a030;"));

	// phone link: online page (works everywhere) and LAN page served by the plugin (precise clock)
	Config c = p.config();
	const QString online = QString::fromStdString(c.publicPageUrl);
	const auto urls = p.phoneUrls();
	const QString lan = urls.empty() ? QString() : QString::fromStdString(urls.front());
	QString html;
	if (!online.isEmpty())
		html += T("Dock.Phone.Online") + QStringLiteral(" <a href=\"%1\">%1</a><br>").arg(online.toHtmlEscaped());
	if (!urls.empty()) {
		html += T("Dock.Phone.Lan");
		for (const auto &u : urls)
			html += QStringLiteral(" <a href=\"%1\">%1</a>").arg(QString::fromStdString(u).toHtmlEscaped());
	} else if (c.serverEnabled) {
		html += QStringLiteral("<span style=\"color:#e05050\">%1</span>")
				.arg(T("Dock.Phone.ServerError").arg(QString::fromStdString(p.serverError()).toHtmlEscaped()));
	}
	if (html != urlLabel_->text())
		urlLabel_->setText(html);
	if (online != qrText_) {
		qrText_ = online;
		qrLabel_->setPixmap(online.isEmpty() ? QPixmap() : makeQr(online, 112));
		qrColumn_->setVisible(!online.isEmpty());
	}
	if (lan != qrLanText_) {
		qrLanText_ = lan;
		qrLanLabel_->setPixmap(lan.isEmpty() ? QPixmap() : makeQr(lan, 112));
		qrLanColumn_->setVisible(!lan.isEmpty());
	}
	QStringList phones;
	for (const auto &cl : p.server().clients()) {
		const double err = json::number(cl.status, "syncErrMs", NAN);
		const double hz = json::number(cl.status, "refreshHz", NAN);
		const bool running = json::boolean(cl.status, "running");
		phones << T("Dock.Phone.Client")
				  .arg(QString::fromStdString(cl.name.empty() ? cl.address : cl.name).toHtmlEscaped(),
				       ms(err), ms(hz, 0), running ? T("Dock.Phone.Running") : T("Dock.Phone.Paused"));
	}
	phonesLabel_->setText(phones.isEmpty() ? T("Dock.Phone.NoClients") : phones.join(QStringLiteral("<br>")));

	auto snap = p.engine().snapshot(60);
	std::stable_sort(snap.begin(), snap.end(), [](const ProbeSnapshot &a, const ProbeSnapshot &b) {
		if (a.kind != b.kind)
			return a.kind == Kind::Video;
		return a.origin == "filter" && b.origin != "filter";
	});
	refreshCombos(snap);
	refreshTable(snap);
	refreshOffset(snap);
	refreshSession();
}

void AvSyncDock::refreshCombos(const std::vector<ProbeSnapshot> &snap)
{
	std::vector<int> vids, aids;
	QStringList vnames, anames;
	for (const auto &s : snap) {
		const QString name = QString::fromStdString(s.name);
		if (s.kind == Kind::Video) {
			vids.push_back(s.id);
			vnames << name;
		} else {
			aids.push_back(s.id);
			anames << name;
		}
	}
	int v, a;
	Plugin::instance().resolvedPair(v, a);
	auto sync = [](QComboBox *combo, std::vector<int> &ids, const std::vector<int> &newIds, const QStringList &names,
		       int current) {
		bool same = ids == newIds && combo->count() == names.size();
		for (int i = 0; same && i < names.size(); i++)
			same = combo->itemText(i) == names[i];
		if (!same) {
			QSignalBlocker block(combo);
			combo->clear();
			combo->addItems(names);
			ids = newIds;
		}
		auto it = std::find(ids.begin(), ids.end(), current);
		const int idx = it == ids.end() ? -1 : (int)(it - ids.begin());
		if (combo->currentIndex() != idx) {
			QSignalBlocker block(combo);
			combo->setCurrentIndex(idx);
		}
	};
	sync(videoCombo_, comboVideoIds_, vids, vnames, v);
	sync(audioCombo_, comboAudioIds_, aids, anames, a);
}

void AvSyncDock::refreshTable(const std::vector<ProbeSnapshot> &snap)
{
	if (table_->rowCount() != (int)snap.size()) {
		table_->setRowCount((int)snap.size());
		int h = table_->horizontalHeader()->sizeHint().height() + 2 * table_->frameWidth();
		for (int r = 0; r < table_->rowCount(); r++)
			h += table_->verticalHeader()->sectionSize(r);
		table_->setFixedHeight(std::max(h, 60));
	}
	auto set = [this](int row, int col, const QString &text, const QString &tip = QString()) {
		QTableWidgetItem *item = table_->item(row, col);
		if (!item) {
			item = new QTableWidgetItem();
			item->setTextAlignment(col == 0 || col == 5 ? Qt::AlignLeft | Qt::AlignVCenter : Qt::AlignRight | Qt::AlignVCenter);
			table_->setItem(row, col, item);
		}
		if (item->text() != text)
			item->setText(text);
		if (item->toolTip() != tip)
			item->setToolTip(tip);
	};
	for (int row = 0; row < (int)snap.size(); row++) {
		const ProbeSnapshot &s = snap[(size_t)row];
		const QString icon = s.kind == Kind::Video ? QStringLiteral("🎥 ") : QStringLiteral("🎤 ");
		const QString name = icon + QString::fromStdString(s.name);
		const bool fresh = s.lastAgeMs >= 0 && s.lastAgeMs < 4000;
		const QString tip = T("Dock.Row.Tip")
					    .arg(ms(s.live.mean), ms(s.live.min), ms(s.live.max), ms(s.total.median))
					    .arg(s.total.count);
		set(row, 0, name, tip);
		set(row, 1, s.live.count ? ms(s.live.last) : QStringLiteral("–"), tip);
		set(row, 2, s.live.count ? ms(s.live.median) : QStringLiteral("–"), tip);
		set(row, 3, s.live.count > 1 ? ms(s.live.stddev) : QStringLiteral("–"), tip);
		set(row, 4, QString::number(s.total.count), tip);
		QString status = QString::fromStdString(s.signal.text);
		if (s.live.count && !s.locked)
			status += QStringLiteral(" · ") + T("Dock.Status.Unverified");
		set(row, 5, status, tip);
		QColor color = fresh ? palette().color(QPalette::Text) : palette().color(QPalette::PlaceholderText);
		for (int col = 0; col < 6; col++)
			if (QTableWidgetItem *item = table_->item(row, col))
				if (item->foreground().color() != color)
					item->setForeground(color);
	}
}

void AvSyncDock::refreshOffset(const std::vector<ProbeSnapshot> &snap)
{
	Plugin &p = Plugin::instance();
	int v, a;
	p.resolvedPair(v, a);
	const ProbeSnapshot *vs = nullptr, *as = nullptr;
	for (const auto &s : snap) {
		if (s.id == v)
			vs = &s;
		if (s.id == a)
			as = &s;
	}
	const size_t window = (size_t)p.config().liveWindow;
	PairStats ps = p.engine().pair(v, a, window);
	if (ps.offset.count == 0) {
		offsetLabel_->setText(QStringLiteral("Δ –"));
		offsetHint_->setText(vs && as ? T("Dock.Offset.Waiting") : T("Dock.Offset.NoPair"));
		applyButton_->setEnabled(false);
	} else {
		const double off = ps.offset.median;
		offsetLabel_->setText(QStringLiteral("Δ %1 ms").arg(signedMs(off)));
		QString hint;
		if (std::fabs(off) < 5.0)
			hint = T("Dock.Offset.InSync");
		else if (off < 0)
			hint = T("Dock.Offset.AudioEarly").arg(ms(-off, 0));
		else
			hint = T("Dock.Offset.AudioLate").arg(ms(off, 0));
		if (ps.relative)
			hint += QStringLiteral("\n") + T("Dock.Offset.Relative");
		else
			hint += QStringLiteral("\n") +
				T("Dock.Offset.Details").arg(ms(vs ? vs->live.median : NAN), ms(as ? as->live.median : NAN),
							     ms(ps.offset.stddev)).arg(ps.offset.count);
		offsetHint_->setText(hint);
		applyButton_->setEnabled(as && as->origin == "filter" && !as->sourceUuid.empty() && ps.offset.count >= 3 &&
					 std::fabs(off) >= 1.0);
	}

	std::vector<LatencyGraph::Series> series;
	if (vs) {
		LatencyGraph::Series s{T("Dock.Graph.Video"), QColor(77, 163, 255), {}};
		for (const auto &m : vs->recent)
			s.points.push_back({m.seq, m.latencyMs});
		series.push_back(std::move(s));
	}
	if (as) {
		LatencyGraph::Series s{T("Dock.Graph.Audio"), QColor(245, 160, 60), {}};
		for (const auto &m : as->recent)
			s.points.push_back({m.seq, m.latencyMs});
		series.push_back(std::move(s));
	}
	if (ps.offset.count) {
		PairStats longer = p.engine().pair(v, a, 60);
		LatencyGraph::Series s{T("Dock.Graph.Offset"), QColor(80, 200, 120), {}};
		for (const auto &r : longer.recent)
			s.points.push_back({r.first, r.second});
		series.push_back(std::move(s));
	}
	graph_->setSeries(std::move(series));
}

void AvSyncDock::refreshSession()
{
	Plugin &p = Plugin::instance();
	SessionResult r = p.engine().session();
	const double now = p.nowUtcMs();
	const QString startText = QDateTime::fromMSecsSinceEpoch((qint64)r.startUtcMs).toString(QStringLiteral("HH:mm:ss"));
	QString text;
	switch (r.state) {
	case SessionState::Idle:
		text = T("Dock.Session.Idle");
		break;
	case SessionState::Scheduled:
		text = T("Dock.Session.Scheduled").arg(startText, ms((r.startUtcMs - now) / 1000.0, 0));
		break;
	case SessionState::Running: {
		const double total = (r.endUtcMs - r.startUtcMs) / 1000.0;
		const double done = std::clamp((now - r.startUtcMs) / 1000.0, 0.0, total);
		text = T("Dock.Session.Running").arg(ms(done, 0), ms(total, 0));
		break;
	}
	case SessionState::Finished: {
		text = T("Dock.Session.Finished").arg(startText);
		for (const auto &row : r.probes) {
			if (!row.stats.count)
				continue;
			text += QStringLiteral("\n%1 %2: Ø %3 ms, Median %4 ms, σ %5 ms (n=%6)")
					.arg(row.kind == Kind::Video ? QStringLiteral("🎥") : QStringLiteral("🎤"),
					     QString::fromStdString(row.name), ms(row.stats.mean), ms(row.stats.median),
					     ms(row.stats.stddev))
					.arg(row.stats.count);
		}
		if (r.pairOffset.count)
			text += QStringLiteral("\n") + T("Dock.Session.Offset").arg(signedMs(r.pairOffset.median), ms(r.pairOffset.stddev));
		const std::string csv = p.lastCsvPath();
		const std::string err = p.lastCsvError();
		if (!csv.empty())
			text += QStringLiteral("\n") + T("Dock.Session.Saved").arg(QString::fromStdString(csv));
		if (!err.empty())
			text += QStringLiteral("\n") + QString::fromStdString(err);
		break;
	}
	}
	if (sessionLabel_->text() != text)
		sessionLabel_->setText(text);
	cancelButton_->setEnabled(r.state == SessionState::Scheduled || r.state == SessionState::Running);
}

AvSyncDock *g_dock = nullptr;

} // namespace

void createDock()
{
	if (g_dock)
		return;
	g_dock = new AvSyncDock();
	g_dock->setObjectName(QStringLiteral("avsyncDock"));
	if (!obs_frontend_add_dock_by_id("avsync-dock", obs_module_text("Dock.Title"), g_dock)) {
		delete g_dock;
		g_dock = nullptr;
		return;
	}
	// test hook: AVSYNC_SHOW_DOCK=1 opens the dock after start-up
	if (getenv("AVSYNC_SHOW_DOCK")) {
		QTimer::singleShot(3000, [] {
			if (g_dock && g_dock->parentWidget()) {
				g_dock->parentWidget()->show();
				g_dock->parentWidget()->raise();
			}
		});
	}
}

void destroyDock()
{
	// The dock belongs to the OBS main window, which deletes it on shutdown.
	g_dock = nullptr;
}

} // namespace avs
