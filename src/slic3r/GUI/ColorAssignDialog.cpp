#include "ColorAssignDialog.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "wxExtensions.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/CheckBox.hpp"
#include "Widgets/ComboBox.hpp"
#include "Widgets/DialogButtons.hpp"
#include "Widgets/Label.hpp"

#include <wx/cursor.h>
#include <wx/sizer.h>
#include <wx/statbox.h>
#include <wx/stattext.h>

#include <algorithm>

namespace Slic3r { namespace GUI {

// A "#RRGGBB" string as wxWidgets wants it. An unreadable value becomes grey
// rather than black, so that "no colour given" does not look like "black".
static wxColour colour_from_hex(const std::string &hex)
{
    wxColour out;
    if (! hex.empty() && out.Set(wxString::FromUTF8(hex.c_str())))
        return out;
    return wxColour(0x9A, 0x9A, 0x9A);
}

// A plain filled rectangle. Deliberately not a ColorPanel from
// DragDropPanel.hpp: that one carries drag and drop behaviour this dialog
// does not want, because dragging cannot be done from the keyboard.
//
// The colour itself is put on in paint_swatches(), after the dark mode pass.
static wxPanel *colour_patch(wxWindow *parent, const wxSize &size)
{
    auto *patch = new wxPanel(parent, wxID_ANY, wxDefaultPosition, size, wxBORDER_SIMPLE);
    patch->SetMinSize(size);
    return patch;
}

ColorAssignDialog::ColorAssignDialog(wxWindow                       *parent,
                                     const wxString                 &filename,
                                     const std::vector<std::string> &model_colours,
                                     const std::vector<Slot>        &slots,
                                     const std::vector<int>         &proposal)
    : DPIDialog(parent, wxID_ANY, _L("Assign the model's colours"), wxDefaultPosition, wxDefaultSize,
                wxCAPTION | wxCLOSE_BOX)
    , m_model_colours(model_colours)
    , m_slot_count(slots.size())
{
    const wxSize patch_size = wxSize(FromDIP(26), FromDIP(16));
    // A swatch that answers to a click is a control, and a control that small
    // is hard to hit. The printer's swatches are therefore bigger than the
    // model's, which only have to be looked at.
    const wxSize swatch_size = wxSize(FromDIP(40), FromDIP(22));

    m_slot_colours.reserve(slots.size());
    for (const Slot &slot : slots)
        m_slot_colours.push_back(slot.colour);
    m_slot_edited.assign(slots.size(), false);

    auto *main = new wxBoxSizer(wxVERTICAL);

    auto *head = new wxStaticText(
        this, wxID_ANY,
        wxString::Format(_L("%s uses %d colours. Your printer has %d filament slots."), filename,
                         int(model_colours.size()), int(slots.size())));
    head->SetFont(::Label::Body_14);
    head->SetMaxSize(wxSize(FromDIP(460), -1));
    head->Wrap(FromDIP(460));
    main->Add(head, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(16));
    main->AddSpacer(FromDIP(10));

    // --- the printer's slots: this is what the model's colours are compared
    // --- against, and the material has to be readable before anyone picks it
    auto *slot_box   = new wxStaticBoxSizer(wxVERTICAL, this, _L("Slots of your printer"));
    auto *slot_grid  = new wxFlexGridSizer(0, 4, FromDIP(6), FromDIP(10));
    slot_grid->AddGrowableCol(2, 1);
    for (size_t i = 0; i < slots.size(); ++i) {
        auto *swatch = colour_patch(slot_box->GetStaticBox(), swatch_size);
        swatch->SetToolTip(_L("Click to choose the colour of this filament slot"));
        swatch->SetCursor(wxCursor(wxCURSOR_HAND));
        swatch->Bind(wxEVT_LEFT_UP, [this, i](wxMouseEvent &) { pick_slot_colour(i); });
        m_slot_patches.push_back(swatch);
        slot_grid->Add(swatch, 0, wxALIGN_CENTER_VERTICAL);

        slot_grid->Add(new wxStaticText(slot_box->GetStaticBox(), wxID_ANY,
                                        wxString::Format("%d", int(i + 1))),
                       0, wxALIGN_CENTER_VERTICAL);

        wxString text = wxString::FromUTF8(slots[i].preset.c_str());
        if (! slots[i].material.empty())
            text += "   " + wxString::FromUTF8(slots[i].material.c_str());
        slot_grid->Add(new wxStaticText(slot_box->GetStaticBox(), wxID_ANY, text), 0,
                       wxALIGN_CENTER_VERTICAL);

        // A click cannot be done from the keyboard, so the same thing gets a
        // button that Tab reaches and Space presses. It carries a word, not
        // an icon: a name a screen reader can read out.
        auto *change = new ::Button(slot_box->GetStaticBox(), _L("Change"));
        change->SetStyle(ButtonStyle::Regular, ButtonType::Compact);
        change->SetToolTip(wxString::Format(_L("Choose the colour of slot %d"), int(i + 1)));
        change->Bind(wxEVT_BUTTON, [this, i](wxCommandEvent &) { pick_slot_colour(i); });
        slot_grid->Add(change, 0, wxALIGN_CENTER_VERTICAL);
    }
    slot_box->Add(slot_grid, 0, wxEXPAND | wxALL, FromDIP(8));

    auto *slot_hint = new wxStaticText(
        slot_box->GetStaticBox(), wxID_ANY,
        _L("The colours below come from the file and cannot be changed here."));
    slot_hint->SetMaxSize(wxSize(FromDIP(440), -1));
    slot_hint->Wrap(FromDIP(440));
    slot_box->Add(slot_hint, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));

    main->Add(slot_box, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(16));
    main->AddSpacer(FromDIP(10));

    // --- the model's colours, each with the slot it goes to
    auto *model_box  = new wxStaticBoxSizer(wxVERTICAL, this, _L("Colours of the model"));
    auto *model_grid = new wxFlexGridSizer(0, 3, FromDIP(4), FromDIP(10));
    model_grid->AddGrowableCol(2, 1);

    wxArrayString slot_labels;
    for (size_t s = 0; s < slots.size(); ++s) {
        wxString label = wxString::Format(_L("Slot %d"), int(s + 1));
        if (! slots[s].material.empty())
            label += "   " + wxString::FromUTF8(slots[s].material.c_str());
        slot_labels.Add(label);
    }

    for (size_t i = 0; i < model_colours.size(); ++i) {
        auto *patch = colour_patch(model_box->GetStaticBox(), patch_size);
        m_model_patches.push_back(patch);
        model_grid->Add(patch, 0, wxALIGN_CENTER_VERTICAL);

        model_grid->Add(new wxStaticText(model_box->GetStaticBox(), wxID_ANY,
                                         wxString::Format("%d", int(i + 1))),
                        0, wxALIGN_CENTER_VERTICAL);

        auto *target = new ComboBox(model_box->GetStaticBox(), wxID_ANY, wxEmptyString,
                                    wxDefaultPosition, wxSize(FromDIP(240), -1), 0, nullptr,
                                    wxCB_READONLY);
        for (const wxString &label : slot_labels)
            target->Append(label);

        // The proposal is a default, never a decision. Where it says nothing,
        // filament i keeps its own number, clamped into the slots that exist.
        int chosen = int(i) + 1;
        if (i < proposal.size() && proposal[i] >= 1)
            chosen = proposal[i];
        chosen = std::min(chosen, int(slots.size()));
        chosen = std::max(chosen, 1);
        target->SetSelection(chosen - 1);

        model_grid->Add(target, 0, wxALIGN_CENTER_VERTICAL);
        m_targets.push_back(target);
    }
    model_box->Add(model_grid, 0, wxEXPAND | wxALL, FromDIP(8));
    main->Add(model_box, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(16));
    main->AddSpacer(FromDIP(10));

    auto *adopt = new ::CheckBox(this);
    adopt->SetValue(false);
    auto *adopt_row = new wxBoxSizer(wxHORIZONTAL);
    adopt_row->Add(adopt, 0, wxALIGN_CENTER_VERTICAL);
    adopt_row->AddSpacer(FromDIP(8));
    auto *adopt_text = new wxStaticText(
        this, wxID_ANY, _L("Also take the model's colours over into the printer's filament slots"));
    adopt_text->SetMaxSize(wxSize(FromDIP(420), -1));
    adopt_text->Wrap(FromDIP(420));
    adopt_row->Add(adopt_text, 1, wxALIGN_CENTER_VERTICAL);
    main->Add(adopt_row, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(16));

    const wxString adopt_tip = _L("A slot whose colour you changed above keeps your colour.");
    adopt->SetToolTip(adopt_tip);
    adopt_text->SetToolTip(adopt_tip);

    adopt->Bind(wxEVT_TOGGLEBUTTON, [this, adopt](wxCommandEvent &e) {
        m_adopt_colours = adopt->GetValue();
        e.Skip();
    });

    main->AddSpacer(FromDIP(10));

    auto *buttons = new DialogButtons(this, {"OK", "Cancel"});
    buttons->GetOK()->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { EndModal(wxID_OK); });
    buttons->GetCANCEL()->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { EndModal(wxID_CANCEL); });
    main->Add(buttons, 0, wxEXPAND);

    SetSizer(main);
    Layout();
    Fit();
    Centre(wxBOTH);
    wxGetApp().UpdateDlgDarkUI(this);

    // Last, so that the dark mode pass cannot recolour a filament. See the
    // note on paint_swatches() in the header.
    paint_swatches();
}

void ColorAssignDialog::paint_swatches()
{
    for (size_t i = 0; i < m_slot_patches.size(); ++i) {
        if (i >= m_slot_colours.size())
            break;
        m_slot_patches[i]->SetBackgroundColour(colour_from_hex(m_slot_colours[i]));
        m_slot_patches[i]->Refresh();
    }
    for (size_t i = 0; i < m_model_patches.size(); ++i) {
        if (i >= m_model_colours.size())
            break;
        m_model_patches[i]->SetBackgroundColour(colour_from_hex(m_model_colours[i]));
        m_model_patches[i]->Refresh();
    }
}

void ColorAssignDialog::pick_slot_colour(size_t index)
{
    if (index >= m_slot_colours.size())
        return;

    wxColour current = colour_from_hex(m_slot_colours[index]);
    if (! current.IsOk())
        current = wxColour(0, 0, 0);
    m_clrData.SetColour(current);

    // Orca's own picker, the one the sidebar opens: "Please choose the
    // filament colour". It returns its argument unchanged when the user
    // cancels, so the answer has to be compared against what went in --
    // exactly what PlaterPresetComboBox::show_default_color_picker() does.
    const wxColourData data = ::show_sys_picker_dialog(this, m_clrData);
    if (data.GetColour() == m_clrData.GetColour())
        return;

    m_clrData.SetColour(data.GetColour());
    m_slot_colours[index] = data.GetColour().GetAsString(wxC2S_HTML_SYNTAX).ToStdString();
    m_slot_edited[index]  = true;

    if (index < m_slot_patches.size()) {
        m_slot_patches[index]->SetBackgroundColour(data.GetColour());
        m_slot_patches[index]->Refresh();
    }
}

std::vector<int> ColorAssignDialog::mapping() const
{
    // Index 0 stands for "take the extruder of the volume" in a painting and
    // must map to itself, so the vector is one longer than the colour list.
    std::vector<int> out(m_model_colours.size() + 1, 0);
    for (size_t i = 0; i < out.size(); ++i)
        out[i] = int(i);

    for (size_t i = 0; i < m_targets.size(); ++i) {
        const int chosen = m_targets[i]->GetSelection();
        if (chosen >= 0 && size_t(chosen) < m_slot_count)
            out[i + 1] = chosen + 1;
    }
    return out;
}

bool ColorAssignDialog::slot_colours_edited() const
{
    for (bool edited : m_slot_edited)
        if (edited)
            return true;
    return false;
}

std::vector<std::string> ColorAssignDialog::final_slot_colours() const
{
    std::vector<std::string> out = m_slot_colours;
    if (! m_adopt_colours)
        return out;

    const std::vector<int> map = mapping();
    for (size_t s = 0; s < out.size(); ++s) {
        if (s < m_slot_edited.size() && m_slot_edited[s])
            continue; // hand beats file
        for (size_t i = 1; i < map.size(); ++i)
            if (map[i] == int(s + 1) && i - 1 < m_model_colours.size()) {
                out[s] = m_model_colours[i - 1];
                break; // two colours on one slot: the first one gets it
            }
    }
    return out;
}

void ColorAssignDialog::on_dpi_changed(const wxRect &suggested_rect)
{
    Fit();
    Refresh();
}

}} // namespace Slic3r::GUI
