/*
 * Copyright 2015 Milian Wolff <mail@milianw.de>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Library General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#include "model.h"

#include <QDebug>
#include <QTextStream>

#include <KFormat>
#include <ThreadWeaver/ThreadWeaver>

#include <sstream>

using namespace std;

namespace {
QString generateSummary(const AccumulatedTraceData& data)
{
    QString ret;
    KFormat format;
    QTextStream stream(&ret);
    const double totalTimeS = 0.001 * data.totalTime;
    stream << "<qt>"
           << "<strong>total runtime</strong>: " << totalTimeS << "s.<br/>"
           << "<strong>bytes allocated in total</strong> (ignoring deallocations): " << format.formatByteSize(data.totalAllocated, 2)
             << " (" << format.formatByteSize(data.totalAllocated / totalTimeS) << "/s)<br/>"
           << "<strong>calls to allocation functions</strong>: " << data.totalAllocations
             << " (" << size_t(data.totalAllocations / totalTimeS) << "/s)<br/>"
           << "<strong>peak heap memory consumption</strong>: " << format.formatByteSize(data.peak) << "<br/>"
           << "<strong>total memory leaked</strong>: " << format.formatByteSize(data.leaked) << "<br/>";
    stream << "</qt>";
    return ret;
}

int parentRow(const QModelIndex& child)
{
    return child.isValid() ? static_cast<int>(child.internalId()) : -1;
}
}

Model::Model(QObject* parent)
    : QAbstractItemModel(parent)
{
}

Model::~Model()
{
}

QVariant Model::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole || section < 0 || section >= NUM_COLUMNS) {
        return {};
    }
    switch (static_cast<Columns>(section)) {
        case FileColumn:
            return tr("File");
        case FunctionColumn:
            return tr("Function");
        case ModuleColumn:
            return tr("Module");
        case AllocationsColumn:
            return tr("Allocations");
        case PeakColumn:
            return tr("Peak");
        case LeakedColumn:
            return tr("Leaked");
        case AllocatedColumn:
            return tr("Allocated");
        case NUM_COLUMNS:
            break;
    }
    return {};
}

QVariant Model::data(const QModelIndex& index, int role) const
{
    if (index.row() < 0 || index.column() < 0 || index.column() > NUM_COLUMNS) {
        return {};
    }
    const auto parent = index.parent();
    if (parent.isValid()) {
        // child level
        if (parent.parent().isValid() || static_cast<size_t>(parent.row()) > m_data.mergedAllocations.size()) {
            return {};
        }
        const auto& allocation = m_data.mergedAllocations[parent.row()];
        const auto& trace = allocation.traces[index.row()];

        if (role == Qt::DisplayRole) {
            auto node = m_data.findTrace(trace.traceIndex);
            // skip first level, it is duplicated on the top-level
            node = m_data.findTrace(node.parentIndex);
            return allocationData(trace, node.ipIndex, static_cast<Columns>(index.column()));
        } else if (role == Qt::ToolTipRole) {
            stringstream stream;
            m_data.printBacktrace(trace.traceIndex, stream);
            return QString::fromStdString(stream.str());
        }
        return {};
    }

    // top-level
    if (static_cast<size_t>(index.row()) > m_data.mergedAllocations.size()) {
        return {};
    }
    const auto& allocation = m_data.mergedAllocations[index.row()];
    if (role == Qt::DisplayRole) {
        return allocationData(allocation, allocation.ipIndex, static_cast<Columns>(index.column()));
    }
    return {};
}

QModelIndex Model::index(int row, int column, const QModelIndex& parent) const
{
    if (row < 0 || column  < 0 || column >= NUM_COLUMNS || row >= rowCount(parent)) {
        return QModelIndex();
    }
    return createIndex(row, column, static_cast<quintptr>(parent.row()));
}

QModelIndex Model::parent(const QModelIndex& child) const
{
    const auto parent = parentRow(child);
    if (parent == -1) {
        return QModelIndex();
    } else {
        return createIndex(parent, 0, -1);
    }
}

int Model::rowCount(const QModelIndex& parent) const
{
    if (parent.isValid()) {
        if (parent.column() != 0 || parent.row() < 0 || static_cast<size_t>(parent.row()) >= m_data.mergedAllocations.size()
            || parentRow(parent) != -1)
        {
            return 0;
        } else {
            return m_data.mergedAllocations[parent.row()].traces.size();
        }
    }
    return m_data.mergedAllocations.size();
}

int Model::columnCount(const QModelIndex& /*parent*/) const
{
    return NUM_COLUMNS;
}

void Model::loadFile(const QString& file)
{
    using namespace ThreadWeaver;
    stream() << make_job([=]() {
        beginResetModel();
        m_data.read(file.toStdString());
        endResetModel();
        emit dataReady(generateSummary(m_data));
    });
}

QVariant Model::allocationData(const AllocationData& allocation, const IpIndex& ipIndex, Columns column) const
{
    switch (column) {
    case AllocationsColumn:
        return static_cast<quint64>(allocation.allocations);
    case PeakColumn:
        return static_cast<quint64>(allocation.peak);
    case LeakedColumn:
        return static_cast<quint64>(allocation.leaked);
    case AllocatedColumn:
        return static_cast<quint64>(allocation.allocated);
    case FileColumn:
    case ModuleColumn:
    case FunctionColumn: {
        const auto& ip = m_data.findIp(ipIndex);
        if (column == FunctionColumn) {
            if (ip.functionIndex) {
                return QString::fromStdString(m_data.prettyFunction(m_data.stringify(ip.functionIndex)));
            } else {
                return static_cast<QString>(QLatin1String("0x") + QString::number(ip.instructionPointer, 16));
            }
        } else if (column == ModuleColumn) {
            return QString::fromStdString(m_data.stringify(ip.moduleIndex));
        } else if (ip.fileIndex) {
            auto file = QString::fromStdString(m_data.stringify(ip.fileIndex));
            return static_cast<QString>(file + QLatin1Char(':') + QString::number(ip.line));
        } else {
            return QString();
        }
        break;
    }
    case NUM_COLUMNS:
        break;
    }
    return {};
}