/****************************************************************************
**
** Copyright (C) 2016 Denis Mingulov
** Contact: https://www.qt.io/licensing/
**
** This file is part of Qt Creator.
**
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3 as published by the Free Software
** Foundation with exceptions as appearing in the file LICENSE.GPL3-EXCEPT
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-3.0.html.
**
****************************************************************************/

#include "classviewutils.h"
#include "classviewconstants.h"
#include "classviewsymbolinformation.h"

// needed for the correct sorting order
#include <cplusplus/Icons.h>

#include <QStandardItem>
#include <QDebug>

namespace ClassView {
namespace Constants {

/*!
   \class Utils
   \brief The Utils class provides some common utilities.
*/

//! Default icon sort order
const int IconSortOrder[] = {
    Utils::CodeModelIcon::Namespace,
    Utils::CodeModelIcon::Enum,
    Utils::CodeModelIcon::Class,
    Utils::CodeModelIcon::FuncPublic,
    Utils::CodeModelIcon::FuncProtected,
    Utils::CodeModelIcon::FuncPrivate,
    Utils::CodeModelIcon::FuncPublicStatic,
    Utils::CodeModelIcon::FuncProtectedStatic,
    Utils::CodeModelIcon::FuncPrivateStatic,
    Utils::CodeModelIcon::Signal,
    Utils::CodeModelIcon::SlotPublic,
    Utils::CodeModelIcon::SlotProtected,
    Utils::CodeModelIcon::SlotPrivate,
    Utils::CodeModelIcon::VarPublic,
    Utils::CodeModelIcon::VarProtected,
    Utils::CodeModelIcon::VarPrivate,
    Utils::CodeModelIcon::VarPublicStatic,
    Utils::CodeModelIcon::VarProtectedStatic,
    Utils::CodeModelIcon::VarPrivateStatic,
    Utils::CodeModelIcon::Enumerator,
    Utils::CodeModelIcon::Keyword,
    Utils::CodeModelIcon::Macro,
    Utils::CodeModelIcon::Unknown
};

} // namespace Constants

namespace Internal {

Utils::Utils() = default;

/*!
    Converts internal location container to QVariant compatible.
    \a locations specifies a set of symbol locations.
    Returns a list of variant locations that can be added to the data of an
    item.
*/

QList<QVariant> Utils::locationsToRole(const QSet<SymbolLocation> &locations)
{
    QList<QVariant> locationsVar;
    foreach (const SymbolLocation &loc, locations)
        locationsVar.append(QVariant::fromValue(loc));

    return locationsVar;
}

/*!
    Converts QVariant location container to internal.
    \a locationsVar contains a list of variant locations from the data of an
    item.
    Returns a set of symbol locations.
 */

QSet<SymbolLocation> Utils::roleToLocations(const QList<QVariant> &locationsVar)
{
    QSet<SymbolLocation> locations;
    foreach (const QVariant &loc, locationsVar) {
        if (loc.canConvert<SymbolLocation>())
            locations.insert(loc.value<SymbolLocation>());
    }

    return locations;
}

/*!
    Returns sort order value for the \a icon.
*/

int Utils::iconTypeSortOrder(int icon)
{
    static QHash<int, int> sortOrder;

    // initialization
    if (sortOrder.isEmpty()) {
        for (int i : Constants::IconSortOrder)
            sortOrder.insert(i, sortOrder.count());
    }

    // if it is missing - return the same value
    if (!sortOrder.contains(icon))
        return icon;

    return sortOrder[icon];
}

/*!
    Sets symbol information specified by \a information to \a item.
    \a information provides the name, type, and icon for the item.
    Returns the filled item.
*/

QStandardItem *Utils::setSymbolInformationToItem(const SymbolInformation &information,
                                                 QStandardItem *item)
{
    Q_ASSERT(item);

    item->setData(information.name(), Constants::SymbolNameRole);
    item->setData(information.type(), Constants::SymbolTypeRole);
    item->setData(information.iconType(), Constants::IconTypeRole);

    return item;
}

/*!
    Returns symbol information for \a item.
*/

SymbolInformation Utils::symbolInformationFromItem(const QStandardItem *item)
{
    Q_ASSERT(item);

    if (!item)
        return SymbolInformation();

    const QString &name = item->data(Constants::SymbolNameRole).toString();
    const QString &type = item->data(Constants::SymbolTypeRole).toString();
    int iconType = 0;

    QVariant var = item->data(Constants::IconTypeRole);
    bool ok = false;
    int value;
    if (var.isValid())
        value = var.toInt(&ok);
    if (ok)
        iconType = value;

    return SymbolInformation(name, type, iconType);
}

/*!
   Updates \a item to \a target, so that it is sorted and can be fetched.
*/

void Utils::fetchItemToTarget(QStandardItem *item, const QStandardItem *target)
{
    if (!item || !target)
        return;

    int itemIndex = 0;
    int targetIndex = 0;
    int itemRows = item->rowCount();
    int targetRows = target->rowCount();

    while (itemIndex < itemRows && targetIndex < targetRows) {
        const QStandardItem *itemChild = item->child(itemIndex);
        const QStandardItem *targetChild = target->child(targetIndex);

        const SymbolInformation &itemInf = symbolInformationFromItem(itemChild);
        const SymbolInformation &targetInf = symbolInformationFromItem(targetChild);

        if (itemInf < targetInf) {
            ++itemIndex;
        } else if (itemInf == targetInf) {
            ++itemIndex;
            ++targetIndex;
        } else {
            item->insertRow(itemIndex, targetChild->clone());
            ++itemIndex;
            ++itemRows;
            ++targetIndex;
        }
    }

    // append
    while (targetIndex < targetRows) {
        item->appendRow(target->child(targetIndex)->clone());
        ++targetIndex;
    }
}

/*!
   Moves \a item to \a target (sorted).
*/
void Utils::moveItemToTarget(QStandardItem *item, const QStandardItem *target)
{
    if (!item || !target)
        return;

    int itemIndex = 0;
    int targetIndex = 0;
    int itemRows = item->rowCount();
    int targetRows = target->rowCount();

    while (itemIndex < itemRows && targetIndex < targetRows) {
        QStandardItem *itemChild = item->child(itemIndex);
        const QStandardItem *targetChild = target->child(targetIndex);

        const SymbolInformation &itemInf = Utils::symbolInformationFromItem(itemChild);
        const SymbolInformation &targetInf = Utils::symbolInformationFromItem(targetChild);

        if (itemInf < targetInf) {
            item->removeRow(itemIndex);
            --itemRows;
        } else if (itemInf == targetInf) {
            moveItemToTarget(itemChild, targetChild);
            ++itemIndex;
            ++targetIndex;
        } else {
            item->insertRow(itemIndex, targetChild->clone());
            moveItemToTarget(item->child(itemIndex), targetChild);
            ++itemIndex;
            ++itemRows;
            ++targetIndex;
        }
    }

    // append
    while (targetIndex < targetRows) {
        item->appendRow(target->child(targetIndex)->clone());
        moveItemToTarget(item->child(itemIndex), target->child(targetIndex));
        ++itemIndex;
        ++itemRows;
        ++targetIndex;
    }

    // remove end of item
    while (itemIndex < itemRows) {
        item->removeRow(itemIndex);
        --itemRows;
    }
}

} // namespace Internal
} // namespace ClassView
