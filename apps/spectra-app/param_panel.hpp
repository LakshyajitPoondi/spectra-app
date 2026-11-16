// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <functional>
#include <vector>

#include <QScrollArea>
#include <QString>
#include <QWidget>

#include <nlohmann/json.hpp>

#include "spectra/data/pack.hpp"

class QComboBox;
class QDoubleSpinBox;
class QFormLayout;
class QSlider;
class QToolButton;
class QVBoxLayout;
class QLabel;

namespace spectra::app {

// Slider with a spin box. The slider covers the useful range; the spin box
// accepts anything the parameter allows.
class NumberEdit : public QWidget {
    Q_OBJECT
public:
    NumberEdit(double soft_min, double soft_max, double hard_min, double hard_max, double step, int decimals,
               QWidget* parent = nullptr);
    double value() const;
    void setValue(double v);  // does not emit
Q_SIGNALS:
    void edited(double v);

private:
    void syncSlider(double v);
    QSlider* slider_;
    QDoubleSpinBox* spin_;
    double lo_, hi_;
};

class Section : public QWidget {
    Q_OBJECT
public:
    Section(const QString& title, bool expanded, QWidget* parent = nullptr);
    QFormLayout* form() const { return form_; }
    void setNote(const QString& text);
    void setExpanded(bool on);

private:
    QToolButton* header_;
    QWidget* body_;
    QFormLayout* form_;
    QLabel* note_ = nullptr;
};

struct RawSettings {
    QString white_balance = "as_shot";
    double temperature = 5500.0, tint = 1.0;
    bool operator==(const RawSettings& o) const {
        return white_balance == o.white_balance && temperature == o.temperature && tint == o.tint;
    }
};

// The parameter side panel. It edits a JSON document with the layout of
// spectra::Params; labels, ranges and tooltips come from the reference GUI's
// manifest (resources/params_manifest.json).
class ParamPanel : public QScrollArea {
    Q_OBJECT
public:
    ParamPanel(std::shared_ptr<const Pack> pack, QWidget* parent = nullptr);

    // doc: current values; baseline: what "reset" returns to (stock defaults).
    void setParams(const nlohmann::json& doc, const nlohmann::json& baseline);
    const nlohmann::json& params() const { return doc_; }
    RawSettings rawSettings() const { return raw_; }
    void setRawSettings(const RawSettings& r);
    void setRawEnabled(bool on);
    void setAdvanced(bool on);

Q_SIGNALS:
    void changed();        // any parameter except the stocks
    void stockChanged();   // film or print profile selected: the caller re-applies stock defaults
    void rawChanged();     // RAW development settings: the file has to be decoded again

private:
    struct Editor {
        std::string path;
        std::function<void(const nlohmann::json&)> set;
        QWidget* widget = nullptr;
        QWidget* label = nullptr;
        bool advanced = false;
    };
    void build();
    Section* addSection(const QString& title, bool expanded);
    void addField(Section* s, const nlohmann::json& spec);
    void addStockRow(Section* s, const QString& label, const std::string& path, const std::string& stage);
    void addDevelopmentTime(Section* s, const std::string& path, const std::string& stock_path, bool advanced,
                            const QString& tooltip);
    void addGroup(const nlohmann::json& group, const QString& title, bool expanded, const QStringList& skip = {});
    void commit(const std::string& path, const nlohmann::json& value);
    void refreshDevelopmentTimes();
    const nlohmann::json* groupByPath(const std::string& path) const;

    std::shared_ptr<const Pack> pack_;
    nlohmann::json manifest_, doc_, baseline_, defaults_;
    std::vector<Editor> editors_;
    std::vector<std::function<void()>> dev_time_refresh_;
    QVBoxLayout* column_ = nullptr;
    RawSettings raw_;
    QComboBox* wb_ = nullptr;
    NumberEdit *temperature_ = nullptr, *tint_ = nullptr;
    Section* raw_section_ = nullptr;
    bool advanced_ = false, updating_ = false;
};

}  // namespace spectra::app
