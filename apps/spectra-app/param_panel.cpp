// SPDX-License-Identifier: GPL-3.0-or-later
#include "param_panel.hpp"

#include <algorithm>
#include <cmath>

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QSignalBlocker>
#include <QSlider>
#include <QToolButton>
#include <QVBoxLayout>

#include "spectra/model/params.hpp"
#include "spectra/pipeline/pipeline.hpp"

namespace spectra::app {

using nlohmann::json;

namespace {

QString pretty(const std::string& leaf) {
    QString s = QString::fromStdString(leaf);
    s.replace('_', ' ');
    if (!s.isEmpty()) s[0] = s[0].toUpper();
    s.replace(" um", " (µm)").replace(" ev", " (EV)").replace(" mm", " (mm)").replace(" rgb", " (R, G, B)");
    s.replace("Usm", "USM").replace(" usm", " USM").replace("Cctf", "CCTF").replace(" cctf", " CCTF");
    return s;
}

QString stock_label(const Profile& p) {
    QString s = QString::fromStdString(p.name.empty() ? p.stock : p.name);
    if (p.name.empty()) {
        s.replace('_', ' ');
        QStringList words = s.split(' ');
        for (QString& w : words)
            if (!w.isEmpty()) w[0] = w[0].toUpper();
        s = words.join(' ');
    }
    return s;
}

// Reset on double click.
class ResetLabel : public QLabel {
public:
    ResetLabel(const QString& text, std::function<void()> reset) : QLabel(text), reset_(std::move(reset)) {
        setToolTip(QObject::tr("Double-click to reset"));
    }

protected:
    void mouseDoubleClickEvent(QMouseEvent*) override {
        if (reset_) reset_();
    }

private:
    std::function<void()> reset_;
};

int decimals_for(double step) {
    if (step >= 1.0) return 1;
    return std::clamp(static_cast<int>(std::ceil(-std::log10(step))) + 1, 2, 4);
}

}  // namespace

// ---- NumberEdit -----------------------------------------------------------------

NumberEdit::NumberEdit(double soft_min, double soft_max, double hard_min, double hard_max, double step, int decimals,
                       QWidget* parent)
    : QWidget(parent), lo_(soft_min), hi_(soft_max) {
    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(6);
    slider_ = new QSlider(Qt::Horizontal);
    slider_->setRange(0, 1000);
    slider_->setFocusPolicy(Qt::ClickFocus);
    spin_ = new QDoubleSpinBox;
    spin_->setRange(hard_min, hard_max);
    spin_->setDecimals(decimals);
    spin_->setSingleStep(step);
    spin_->setKeyboardTracking(false);
    spin_->setFixedWidth(82);
    row->addWidget(slider_, 1);
    row->addWidget(spin_);
    connect(slider_, &QSlider::valueChanged, this, [this](int t) {
        double v = lo_ + (hi_ - lo_) * t / 1000.0;
        // snap to the spin box's resolution
        double q = std::pow(10.0, spin_->decimals());
        v = std::round(v * q) / q;
        QSignalBlocker b(spin_);
        spin_->setValue(v);
        Q_EMIT edited(spin_->value());
    });
    connect(spin_, &QDoubleSpinBox::valueChanged, this, [this](double v) {
        syncSlider(v);
        Q_EMIT edited(v);
    });
}

double NumberEdit::value() const { return spin_->value(); }

void NumberEdit::syncSlider(double v) {
    QSignalBlocker b(slider_);
    slider_->setValue(static_cast<int>(std::lround(std::clamp((v - lo_) / (hi_ - lo_), 0.0, 1.0) * 1000.0)));
}

void NumberEdit::setValue(double v) {
    QSignalBlocker b(spin_);
    spin_->setValue(v);
    syncSlider(v);
}

// ---- Section ----------------------------------------------------------------------

Section::Section(const QString& title, bool expanded, QWidget* parent) : QWidget(parent) {
    auto* col = new QVBoxLayout(this);
    col->setContentsMargins(0, 0, 0, 0);
    col->setSpacing(0);
    header_ = new QToolButton;
    header_->setText(title);
    header_->setCheckable(true);
    header_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    header_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    header_->setObjectName("sectionHeader");
    body_ = new QWidget;
    form_ = new QFormLayout(body_);
    form_->setContentsMargins(10, 6, 6, 10);
    form_->setHorizontalSpacing(10);
    form_->setVerticalSpacing(5);
    form_->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form_->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    col->addWidget(header_);
    col->addWidget(body_);
    connect(header_, &QToolButton::toggled, this, [this](bool on) {
        body_->setVisible(on);
        header_->setArrowType(on ? Qt::DownArrow : Qt::RightArrow);
    });
    header_->setChecked(expanded);
    body_->setVisible(expanded);
    header_->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
}

void Section::setExpanded(bool on) { header_->setChecked(on); }

void Section::setNote(const QString& text) {
    if (!note_) {
        note_ = new QLabel;
        note_->setWordWrap(true);
        note_->setObjectName("sectionNote");
        form_->insertRow(0, note_);
    }
    note_->setText(text);
}

// ---- ParamPanel ---------------------------------------------------------------------

ParamPanel::ParamPanel(std::shared_ptr<const Pack> pack, QWidget* parent) : QScrollArea(parent), pack_(std::move(pack)) {
    QFile f(":/params_manifest.json");
    if (f.open(QIODevice::ReadOnly)) manifest_ = json::parse(f.readAll().toStdString(), nullptr, false);
    if (!manifest_.is_object()) manifest_ = json{{"groups", json::array()}, {"panels", json::object()}};
    defaults_ = params_to_json(Params{});
    doc_ = defaults_;
    baseline_ = defaults_;
    auto* host = new QWidget;
    column_ = new QVBoxLayout(host);
    column_->setContentsMargins(4, 4, 4, 4);
    column_->setSpacing(2);
    setWidget(host);
    setWidgetResizable(true);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setFrameShape(QFrame::NoFrame);
    build();
    column_->addStretch(1);
    setAdvanced(false);
}

Section* ParamPanel::addSection(const QString& title, bool expanded) {
    auto* s = new Section(title, expanded);
    column_->addWidget(s);
    return s;
}

const json* ParamPanel::groupByPath(const std::string& path) const {
    for (const json& g : manifest_.at("groups"))
        if (g.value("path", "") == path) return &g;
    return nullptr;
}

void ParamPanel::commit(const std::string& path, const json& value) {
    if (updating_) return;
    doc_[json::json_pointer(path)] = value;
    if (path == "/film" || path == "/print") {
        Q_EMIT stockChanged();
    } else {
        Q_EMIT changed();
    }
}

void ParamPanel::addStockRow(Section* s, const QString& label, const std::string& path, const std::string& stage) {
    auto* combo = new QComboBox;
    combo->setMaxVisibleItems(30);
    for (const std::string& id : list_profiles(*pack_)) {
        Profile p = load_profile(*pack_, id);
        if (p.stage != stage) continue;
        QString text = stock_label(p);
        QStringList tags;
        if (p.is_bw()) tags << tr("B&W");
        if (stage == "filming") tags << (p.is_positive() ? tr("slide") : tr("negative"));
        if (p.is_cine()) tags << tr("cine");
        if (!tags.isEmpty()) text += "  (" + tags.join(", ") + ")";
        combo->addItem(text, QString::fromStdString(id));
    }
    connect(combo, &QComboBox::currentIndexChanged, this, [this, combo, path](int) {
        commit(path, combo->currentData().toString().toStdString());
    });
    s->form()->addRow(label, combo);
    Editor e;
    e.path = path;
    e.widget = combo;
    e.set = [combo](const json& v) {
        int i = combo->findData(QString::fromStdString(v.get<std::string>()));
        if (i >= 0) combo->setCurrentIndex(i);
    };
    editors_.push_back(e);
}

void ParamPanel::addDevelopmentTime(Section* s, const std::string& path, const std::string& stock_path, bool advanced,
                                    const QString& tooltip) {
    auto* combo = new QComboBox;
    combo->setToolTip(tooltip);
    auto* label = new QLabel(tr("Development time"));
    label->setToolTip(tooltip);
    s->form()->addRow(label, combo);
    connect(combo, &QComboBox::currentIndexChanged, this, [this, combo, path](int i) {
        if (i <= 0) commit(path, nullptr);
        else commit(path, combo->currentData().toDouble());
    });
    auto refresh = [this, combo, path, stock_path] {
        QSignalBlocker b(combo);
        combo->clear();
        combo->addItem(QString::fromUtf8("—"));
        try {
            Profile p = load_profile(*pack_, doc_.at(json::json_pointer(stock_path)).get<std::string>());
            for (double t : p.development_time) combo->addItem(tr("%1 min").arg(t), t);
        } catch (const std::exception&) {
        }
        combo->setEnabled(combo->count() > 1);
        const json& v = doc_.at(json::json_pointer(path));
        int idx = 0;
        if (v.is_number())
            for (int i = 1; i < combo->count(); ++i)
                if (std::fabs(combo->itemData(i).toDouble() - v.get<double>()) < 1e-9) idx = i;
        combo->setCurrentIndex(idx);
    };
    dev_time_refresh_.push_back(refresh);
    Editor e;
    e.path = path;
    e.widget = combo;
    e.label = label;
    e.advanced = advanced;
    e.set = [refresh](const json&) { refresh(); };
    editors_.push_back(e);
}

void ParamPanel::addField(Section* s, const json& spec) {
    const std::string path = spec.at("path").get<std::string>();
    json::json_pointer ptr(path);
    if (!defaults_.contains(ptr)) return;  // not a parameter of this app (reference GUI state)
    const json def = defaults_.at(ptr);
    const std::string leaf = path.substr(path.find_last_of('/') + 1);
    QString label = spec.contains("label") ? QString::fromStdString(spec.at("label").get<std::string>())
                                           : (leaf == "active" ? tr("Enabled") : pretty(leaf));
    QString tip = QString::fromStdString(spec.value("tooltip", ""));
    bool advanced = spec.value("tier", "dev") != "basic";
    Editor e;
    e.path = path;
    e.advanced = advanced;
    auto reset = [this, path] {
        json::json_pointer p(path);
        if (!baseline_.contains(p)) return;
        doc_[p] = baseline_.at(p);
        for (auto& ed : editors_)
            if (ed.path == path) {
                updating_ = true;
                ed.set(doc_.at(p));
                updating_ = false;
            }
        Q_EMIT changed();
    };
    auto* lab = new ResetLabel(label, reset);
    if (!tip.isEmpty()) lab->setToolTip(tip + tr("\n(Double-click the name to reset.)"));
    e.label = lab;

    if (leaf == "development_time") {
        std::string stock = path.rfind("/film_render", 0) == 0 ? "/film" : "/print";
        addDevelopmentTime(s, path, stock, advanced, tip);
        return;
    }
    if (def.is_boolean()) {
        auto* box = new QCheckBox;
        box->setToolTip(tip);
        connect(box, &QCheckBox::toggled, this, [this, path](bool on) { commit(path, on); });
        e.widget = box;
        e.set = [box](const json& v) { box->setChecked(v.get<bool>()); };
    } else if (def.is_string()) {
        auto* combo = new QComboBox;
        combo->setToolTip(tip);
        if (spec.contains("choices"))
            for (const json& c : spec.at("choices")) {
                QString id = QString::fromStdString(c.get<std::string>());
                QString text = id;
                text.replace('_', ' ');
                combo->addItem(text, id);
            }
        connect(combo, &QComboBox::currentIndexChanged, this, [this, combo, path](int) {
            commit(path, combo->currentData().toString().toStdString());
        });
        e.widget = combo;
        e.set = [combo](const json& v) {
            QString id = QString::fromStdString(v.get<std::string>());
            int i = combo->findData(id);
            if (i < 0) {
                combo->addItem(id, id);
                i = combo->count() - 1;
            }
            combo->setCurrentIndex(i);
        };
    } else if (def.is_number()) {
        double d = def.get<double>();
        bool integer = def.is_number_integer();
        double step = spec.value("step", 0.0);
        double hard_lo = spec.value("min", -1e6), hard_hi = spec.value("max", 1e6);
        double lo = spec.contains("min") ? hard_lo : std::min(0.0, d * 4);
        double hi;
        if (spec.contains("max")) hi = hard_hi;
        else if (step > 0) hi = std::max({d * 3.0, lo + step * 40, d + step * 20});
        else hi = d > 0 ? d * 4 : 1.0;
        // Useful slider spans where the manifest only gives formal limits.
        if (leaf == "exposure_compensation_ev") lo = -5, hi = 5;
        if (leaf == "y_filter_shift" || leaf == "m_filter_shift") lo = -60, hi = 60;
        if (leaf == "preflash_y_filter_shift" || leaf == "preflash_m_filter_shift") lo = -60, hi = 60;
        if (leaf == "print_exposure") lo = 0, hi = 3;
        if (leaf == "film_format_mm") lo = 8, hi = 120;
        if (lo >= hi) hi = lo + 1;
        if (step <= 0) step = integer ? 1.0 : (hi - lo) / 100.0;
        int dec = integer ? 0 : spec.value("decimals", decimals_for(step));
        auto* num = new NumberEdit(lo, hi, hard_lo, hard_hi, step, dec);
        num->setToolTip(tip);
        connect(num, &NumberEdit::edited, this, [this, path, integer](double v) {
            if (integer) commit(path, static_cast<long>(std::lround(v)));
            else commit(path, v);
        });
        e.widget = num;
        e.set = [num](const json& v) {
            if (v.is_number()) num->setValue(v.get<double>());
        };
    } else if (def.is_array() && !def.empty() && def[0].is_number()) {
        auto* host = new QWidget;
        auto* row = new QHBoxLayout(host);
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(4);
        double step = spec.value("step", 0.0);
        std::vector<QDoubleSpinBox*> boxes;
        for (size_t i = 0; i < def.size(); ++i) {
            auto* sb = new QDoubleSpinBox;
            double d = def[i].get<double>();
            double st = step > 0 ? step : std::max(std::fabs(d) / 20.0, 0.01);
            sb->setRange(spec.value("min", -1e6), spec.value("max", 1e6));
            sb->setDecimals(spec.value("decimals", decimals_for(st)));
            sb->setSingleStep(st);
            sb->setKeyboardTracking(false);
            sb->setToolTip(tip);
            row->addWidget(sb, 1);
            boxes.push_back(sb);
        }
        for (auto* sb : boxes)
            connect(sb, &QDoubleSpinBox::valueChanged, this, [this, path, boxes](double) {
                json a = json::array();
                for (auto* b : boxes) a.push_back(b->value());
                commit(path, a);
            });
        e.widget = host;
        e.set = [boxes](const json& v) {
            if (!v.is_array()) return;
            for (size_t i = 0; i < boxes.size() && i < v.size(); ++i) {
                QSignalBlocker b(boxes[i]);
                boxes[i]->setValue(v[i].get<double>());
            }
        };
    } else {
        delete lab;
        return;  // optional / unsupported type: not editable here
    }
    s->form()->addRow(e.label, e.widget);
    editors_.push_back(e);
}

void ParamPanel::addGroup(const json& group, const QString& title, bool expanded, const QStringList& skip) {
    Section* s = addSection(title, expanded);
    for (const json& f : group.at("fields")) {
        std::string path = f.at("path").get<std::string>();
        QString leaf = QString::fromStdString(path.substr(path.find_last_of('/') + 1));
        if (skip.contains(leaf)) continue;
        addField(s, f);
    }
}

void ParamPanel::build() {
    const json& panels = manifest_.at("panels");
    auto panel = [&](const char* name) -> const json& {
        static const json empty = json::array();
        return panels.contains(name) ? panels.at(name) : empty;
    };
    auto group = [&](const char* path, const QString& title, bool expanded = false, const QStringList& skip = {}) {
        if (const json* g = groupByPath(path)) addGroup(*g, title, expanded, skip);
    };
    auto own = [&](Section* s, const char* path, const char* label, const char* tip, bool basic, json extra = json::object()) {
        json spec = extra;
        spec["path"] = path;
        spec["label"] = label;
        spec["tooltip"] = tip;
        spec["tier"] = basic ? "basic" : "dev";
        addField(s, spec);
    };

    // -- stocks
    Section* stocks = addSection(tr("Film and print"), true);
    addStockRow(stocks, tr("Film"), "/film", "filming");
    addStockRow(stocks, tr("Print"), "/print", "printing");
    for (const json& f : panel("SIMULATION_FOOTER_FIELDS")) {
        json spec = f;
        spec["tier"] = "basic";
        addField(stocks, spec);
    }
    for (const json& f : panel("SIMULATION_SPECIAL_BORROWED_FIELDS")) {
        json spec = f;
        spec["tier"] = "dev";
        addField(stocks, spec);
    }

    // -- exposure and printing: the controls used most
    Section* exposure = addSection(tr("Exposure and print"), true);
    if (const json* cam = groupByPath("/camera"))
        for (const json& f : cam->at("fields")) addField(exposure, f);
    own(exposure, "/camera/lens_blur_um", "Lens blur (µm)", "Sigma of the camera lens blur on the film, in micrometres.",
        false, {{"min", 0}, {"step", 0.5}});
    for (const json& f : panel("SIMULATION_EXPOSURE_PANEL_FIELDS")) {
        json spec = f;
        spec["tier"] = "basic";
        addField(exposure, spec);
    }
    for (const json& f : panel("SIMULATION_ENLARGER_PANEL_FIELDS")) {
        json spec = f;
        spec["tier"] = "basic";
        addField(exposure, spec);
    }
    own(exposure, "/enlarger/lens_blur", "Enlarger lens blur", "Sigma of the enlarger lens blur, in pixels.", false,
        {{"min", 0}, {"step", 0.1}});

    group("/film_render/grain", tr("Grain"));
    group("/film_render/halation", tr("Halation"));
    group("/film_render/dir_couplers", tr("Couplers"));
    group("/film_render/chemistry", tr("Film chemistry"));
    group("/film_render/base", tr("Film base"));
    group("/camera/diffusion_filter", tr("Camera diffusion filter"));
    group("/enlarger", tr("Preflash"));
    group("/enlarger/diffusion_filter", tr("Enlarger diffusion filter"));
    group("/print_render/chemistry", tr("Print chemistry"));
    group("/print_render/glare", tr("Print glare"));
    group("/scanner", tr("Scanner"));

    // -- colour management
    Section* io = addSection(tr("Input and output colour"), false);
    for (const json& f : panel("INPUT_PANEL_FIELDS")) {
        json spec = f;
        spec["tier"] = "dev";
        addField(io, spec);
    }
    if (const json* g = groupByPath("/io/input_gamut_compress"))
        for (const json& f : g->at("fields")) {
            json spec = f;
            std::string leaf = f.at("path").get<std::string>();
            leaf = leaf.substr(leaf.find_last_of('/') + 1);
            spec["label"] = "Input gamut: " + pretty(leaf == "active" ? "enabled" : leaf).toLower().toStdString();
            spec["tier"] = "dev";
            addField(io, spec);
        }
    for (const json& f : panel("SPECTRAL_PANEL_FIELDS")) addField(io, f);
    if (const json* g = groupByPath("/io/output_gamut_compress"))
        for (const json& f : g->at("fields")) {
            json spec = f;
            std::string leaf = f.at("path").get<std::string>();
            leaf = leaf.substr(leaf.find_last_of('/') + 1);
            spec["label"] = "Output gamut: " + pretty(leaf).toLower().toStdString();
            addField(io, spec);
        }
    for (const json& f : panel("SIMULATION_OUTPUT_PANEL_FIELDS")) {
        json spec = f;
        spec["label"] = "Export colour space";
        spec["tooltip"] = "Colour space of exported files. The preview is always shown in sRGB.";
        spec["tier"] = "basic";
        addField(io, spec);
    }
    own(io, "/io/output_cctf_encoding", "Export transfer function",
        "Encode exported files with the colour space's transfer function (off = linear).", false);

    // -- Extended: not part of spektrafilm
    Section* ext = addSection(tr("Extended (not part of spektrafilm)"), false);
    ext->setNote(tr("These controls are additions adapted from the spektrafilm-ofx plugin. They are not part of the "
                    "spektrafilm model, are off by default, and are not covered by the parity tests."));
    own(ext, "/extended/push_pull/active", "Push / pull", "Simulate push or pull processing.", true);
    own(ext, "/extended/push_pull/mode", "Push / pull model",
        "standard: development changes the curve gamma about mid-grey. experimental: per-layer, per-tone warp.", true,
        {{"choices", {"standard", "experimental"}}});
    own(ext, "/extended/push_pull/film_stops", "Film push (stops)", "Positive pushes, negative pulls (-2 to +2).", true,
        {{"min", -2}, {"max", 2}, {"step", 0.25}});
    own(ext, "/extended/push_pull/print_stops", "Print push (stops)", "Print / paper development, gamma only.", true,
        {{"min", -2}, {"max", 2}, {"step", 0.25}});
    own(ext, "/extended/bleach_bypass/active", "Bleach bypass", "Retain part of the silver image.", true);
    own(ext, "/extended/bleach_bypass/film_amount", "Bleach bypass: film",
        "Fraction of the silver image retained in the film.", true, {{"min", 0}, {"max", 1}, {"step", 0.05}});
    own(ext, "/extended/bleach_bypass/print_amount", "Bleach bypass: print",
        "Fraction of the silver image retained in the print.", true, {{"min", 0}, {"max", 1}, {"step", 0.05}});
    own(ext, "/extended/bleach_bypass/leuco_cyan_coupling", "Leuco cyan loss",
        "Negative film only: cyan dye loss multiplier of a skipped bleach.", false,
        {{"min", 0}, {"max", 2}, {"step", 0.05}});
    own(ext, "/extended/retime_print_to_midgray", "Re-time print to mid-grey",
        "Print a pushed or bleach-bypassed negative so that mid-grey keeps its density and colour, as a lab does.",
        false);
    own(ext, "/extended/hdr_output/active", "HDR export", "Export with an HDR transfer function. Export only.", true);
    own(ext, "/extended/hdr_output/transfer", "HDR transfer", "pq: SMPTE ST 2084. hlg: ARIB STD-B67.", true,
        {{"choices", {"pq", "hlg"}}});
    own(ext, "/extended/hdr_output/reference_white_nits", "Reference white (nits)",
        "Luminance of output value 1.0 (print white).", false, {{"min", 80}, {"max", 400}, {"step", 1}});
    own(ext, "/extended/hdr_output/peak_nits", "Peak (nits)", "Peak luminance of the target display.", false,
        {{"min", 400}, {"max", 10000}, {"step", 100}});
    own(ext, "/extended/hdr_output/exposure_ev", "HDR exposure (EV)", "Exposure offset before the HDR encoding.", false,
        {{"min", -4}, {"max", 4}, {"step", 0.25}});
    own(ext, "/extended/hdr_output/tone_mapping", "HDR highlights", "shoulder: soft roll-off to the peak. clip: hard clip.",
        false, {{"choices", {"shoulder", "clip"}}});

    // -- RAW development
    raw_section_ = addSection(tr("RAW development"), false);
    wb_ = new QComboBox;
    wb_->addItem(tr("As shot"), "as_shot");
    wb_->addItem(tr("Daylight"), "daylight");
    wb_->addItem(tr("Tungsten"), "tungsten");
    wb_->addItem(tr("Custom"), "custom");
    temperature_ = new NumberEdit(2500, 10000, 1500, 25000, 50, 0);
    tint_ = new NumberEdit(0.5, 1.5, 0.1, 4.0, 0.01, 2);
    raw_section_->form()->addRow(tr("White balance"), wb_);
    raw_section_->form()->addRow(tr("Temperature (K)"), temperature_);
    raw_section_->form()->addRow(tr("Tint"), tint_);
    auto raw_edit = [this] {
        if (updating_) return;
        raw_.white_balance = wb_->currentData().toString();
        raw_.temperature = temperature_->value();
        raw_.tint = tint_->value();
        bool custom = raw_.white_balance == "custom";
        temperature_->setEnabled(custom);
        tint_->setEnabled(custom);
        Q_EMIT rawChanged();
    };
    connect(wb_, &QComboBox::currentIndexChanged, this, raw_edit);
    connect(temperature_, &NumberEdit::edited, this, raw_edit);
    connect(tint_, &NumberEdit::edited, this, raw_edit);
    setRawSettings(raw_);
}

void ParamPanel::setRawSettings(const RawSettings& r) {
    updating_ = true;
    raw_ = r;
    int i = wb_->findData(r.white_balance);
    wb_->setCurrentIndex(i < 0 ? 0 : i);
    temperature_->setValue(r.temperature);
    tint_->setValue(r.tint);
    temperature_->setEnabled(r.white_balance == "custom");
    tint_->setEnabled(r.white_balance == "custom");
    updating_ = false;
}

void ParamPanel::setRawEnabled(bool on) { raw_section_->setEnabled(on); }

void ParamPanel::setParams(const json& doc, const json& baseline) {
    doc_ = doc;
    baseline_ = baseline;
    updating_ = true;
    for (auto& e : editors_) {
        json::json_pointer p(e.path);
        if (!doc_.contains(p)) continue;
        QSignalBlocker b(e.widget);
        e.set(doc_.at(p));
    }
    updating_ = false;
}

void ParamPanel::setAdvanced(bool on) {
    advanced_ = on;
    for (auto& e : editors_) {
        bool show = on || !e.advanced;
        if (e.widget) e.widget->setVisible(show);
        if (e.label) e.label->setVisible(show);
    }
}

}  // namespace spectra::app
