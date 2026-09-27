// camera_selector.cpp
//
// Camera enumeration + the pre-GUI selector dialog (see camera_selector.h).
//
// The enumeration only asks the SDK (no camera is opened), so it is safe to run
// before the worker thread starts and even while another process holds a
// camera. The dialog is a plain QDialog with a QListWidget + a single Cancel
// button (there is no Ok: clicking a row confirms, so it was redundant); the
// app-wide dark stylesheet already styles the button and the labels, the list
// gets its own scoped rules (see below). Clicking a row confirms that camera
// immediately (Enter also confirms the current row; Cancel/Esc exit).

#include "camera_selector.h"

#include <ASICamera2.h>

#include <QAbstractItemView>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLabel>
#include <QListWidget>
#include <QKeySequence>
#include <QListWidgetItem>
#include <QPushButton>
#include <QShortcut>
#include <QVBoxLayout>

#include <cstdio>

std::vector<CameraOption> enumerateCameras()
{
    std::vector<CameraOption> cams;
    const int n = ASIGetNumOfConnectedCameras();
    for (int i = 0; i < n; ++i)
    {
        ASI_CAMERA_INFO info = {};
        if (ASIGetCameraProperty(&info, i) != ASI_SUCCESS)
        {
            // The SDK still COUNTS this camera, but it cannot be
            // described: the documented symptom of the missing ZWO camera
            // driver (docs/hardware.md). Offer only the cameras the app can
            // actually open.
            std::fprintf(stderr, "[cam] camera at index %d is connected but unreadable "
                                 "(ZWO driver missing or not bound) - not offering it\n", i);
            continue;
        }
        CameraOption c;
        c.index = i;
        c.id = info.CameraID;
        c.name = QString::fromLocal8Bit(info.Name);
        c.detail = QString("%1 · %2×%3")
                       .arg(info.IsColorCam ? "colour" : "mono")
                       .arg(info.MaxWidth)
                       .arg(info.MaxHeight);
        cams.push_back(c);
    }
    return cams;
}

int resolveCameraValue(const std::vector<CameraOption>& cams, const QString& value)
{
    const long num = value.toLong();
    for (const auto& c : cams)
        if (c.id == num) return c.id;      // an exact CameraID wins
    for (const auto& c : cams)
        if (c.index == num) return c.id;   // otherwise a list index
    return -1;
}

int showCameraSelector(const std::vector<CameraOption>& cams)
{
    if (cams.empty()) return -1;
    if (cams.size() == 1) return cams[0].id;   // defensive: nothing to choose

    QDialog dlg;
    dlg.setObjectName("camSel");
    dlg.setWindowTitle("Select camera");
    dlg.setStyleSheet(R"CSS(
        QDialog#camSel { background: #232323; }
        QLabel#camSelHead { color: #e0e0e0; font-size: 16px; }
        QListWidget#camSelList {
            background: #2b2b2b; border: none; border-radius: 10px;
            color: #ffffff; font-size: 17px; font-weight: 600;
            outline: none; padding: 4px;
        }
        QListWidget#camSelList::item { padding: 10px 12px; margin: 2px 4px;
                                       border-radius: 8px; }
        QListWidget#camSelList::item:hover { background: #3a3a3a; }
        QListWidget#camSelList::item:selected { background: #1976d2; color: #ffffff; }
        /* Cancel: a touch-friendly footprint (there is no Ok button -
           clicking a row confirms, so it was redundant). */
        QDialog#camSel QPushButton { min-width: 190px; min-height: 60px; }
    )CSS");

    auto* lay = new QVBoxLayout(&dlg);
    lay->setContentsMargins(20, 20, 20, 18);
    lay->setSpacing(12);

    auto* head = new QLabel(
        "More than one ASI camera is connected.\nClick the one the app should open", &dlg);
    head->setObjectName("camSelHead");
    head->setAlignment(Qt::AlignHCenter);
    head->setWordWrap(true);
    lay->addWidget(head);

    auto* list = new QListWidget(&dlg);
    list->setObjectName("camSelList");
    list->setSelectionMode(QAbstractItemView::SingleSelection);
    list->setCursor(Qt::PointingHandCursor);
    for (const auto& c : cams)
    {
        auto* item = new QListWidgetItem(
            QString("%1.  %2   (%3)").arg(c.index).arg(c.name).arg(c.detail), list);
        item->setData(Qt::UserRole, c.id);
    }
    list->setCurrentRow(0);
    lay->addWidget(list, 1);

    // A single Cancel button: the Ok button was removed as redundant once
    // clicking a row confirms that camera (see below). Esc still cancels.
    auto* btns = new QDialogButtonBox(QDialogButtonBox::Cancel, &dlg);
    lay->addWidget(btns);

    QObject::connect(btns, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    // Enter confirms the currently selected row (the keyboard path that the
    // Ok button used to cover).
    auto* enterKey = new QShortcut(QKeySequence(Qt::Key_Return), &dlg);
    QObject::connect(enterKey, &QShortcut::activated, &dlg, &QDialog::accept);
    // A click on a row confirms THAT camera (select + open in one gesture):
    // the usual way to pick here is pointing at the body, not row-then-button.
    // Enter still confirms the currently selected row for keyboard use.
    QObject::connect(list, &QListWidget::itemClicked, &dlg,
                     [&dlg, list](QListWidgetItem* it) {
                         list->setCurrentItem(it);
                         dlg.accept();
                     });

    dlg.setMinimumWidth(440);

    if (dlg.exec() != QDialog::Accepted) return -1;
    const auto* cur = list->currentItem();
    return (cur && cur->data(Qt::UserRole).isValid()) ? cur->data(Qt::UserRole).toInt() : -1;
}
