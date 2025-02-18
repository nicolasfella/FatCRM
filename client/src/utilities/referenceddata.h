/*
  This file is part of FatCRM, a desktop application for SugarCRM written by KDAB.

  Copyright (C) 2015-2021 Klarälvdalens Datakonsult AB, a KDAB Group company, info@kdab.com
  Authors: David Faure <david.faure@kdab.com>
           Michel Boyer de la Giroday <michel.giroday@kdab.com>
           Kevin Krammer <kevin.krammer@kdab.com>

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 2 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef REFERENCEDDATA_H
#define REFERENCEDDATA_H

#include "enums.h"
#include "fatcrmprivate_export.h"

#include <QObject>

template <typename K, typename V> class QMap;

struct KeyValue
{
    explicit KeyValue(const QString &k = QString(), const QString &v = QString())
        : key(k), value(v) {}
    QString key;
    QString value;
    bool operator<(const KeyValue &other) const { return key < other.key; }
    static bool lessThan(const KeyValue &first, const KeyValue &other) { return first.key < other.key; }
};

/**
 * This class contains the reference data (id+name of contacts, accounts, etc.)
 * for comboboxes (accounts list, assigned-to list, etc.)
 * @brief Per-type singleton holding all reference data, for comboboxes
 * (accounts list, assigned-to list, etc.)
 *
 * Used with a ReferencedDataModel on top.
 */
class FATCRMPRIVATE_EXPORT ReferencedData : public QObject
{
    Q_OBJECT

public:
    /**
     * @brief Per-type singleton holding the full list of all contacts or accounts.
     */
    static ReferencedData *instance(ReferencedDataType type);
    /**
     * Constructor, only public for the restricted data sets
     * (e.g. the few accounts for an account, as shown in an opportunity)
     */
    explicit ReferencedData(ReferencedDataType type, QObject *parent = nullptr);

    /**
     * Clears all the per-type singletons
     */
    static void clearAll();

    ~ReferencedData() override;

    void clear();

    void setReferencedData(const QString &id, const QString &data);
    void addMap(const QMap<QString, QString> &idDataMap, bool emitChanges);
    void removeReferencedData(const QString &id, bool emitChanges);

    QString referencedData(const QString &id) const;

    KeyValue data(int row) const;
    int count() const;

    ReferencedDataType dataType() const;

    void emitInitialLoadingDone();

    static void emitInitialLoadingDoneForAll();

Q_SIGNALS:
    void dataChanged(int row);
    void rowsAboutToBeInserted(int start, int end);
    void rowsInserted();
    void rowsAboutToBeRemoved(int start, int end);
    void rowsRemoved();
    void cleared();

    void initialLoadingDone();

private:
    void setReferencedDataInternal(const QString &id, const QString &data, bool emitChanges);

private:
    class Private;
    Private *const d;
};

#endif
