/*
 *  Copyright (c) 2008 Boudewijn Rempt <boud@valdyas.org>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#include "kis_generator_layer.h"

#include <klocalizedstring.h>
#include "kis_debug.h"

#include <KoIcon.h>
#include <kis_icon.h>
#include "kis_selection.h"
#include "filter/kis_filter_configuration.h"
#include "kis_processing_information.h"
#include "generator/kis_generator_registry.h"
#include "generator/kis_generator.h"
#include "kis_node_visitor.h"
#include "kis_processing_visitor.h"
#include "kis_thread_safe_signal_compressor.h"
#include "kis_recalculate_generator_layer_job.h"
#include "kis_generator_stroke_strategy.h"
#include "krita_utils.h"

#define UPDATE_DELAY 100 /*ms */

struct Q_DECL_HIDDEN KisGeneratorLayer::Private
{
    Private()
        : updateSignalCompressor(UPDATE_DELAY, KisSignalCompressor::FIRST_INACTIVE)
    {
    }

    KisThreadSafeSignalCompressor updateSignalCompressor;
    QRect preparedRect;
    KisFilterConfigurationSP preparedForFilter;
    KisStrokeId strokeId;
};


KisGeneratorLayer::KisGeneratorLayer(KisImageWSP image,
                                     const QString &name,
                                     KisFilterConfigurationSP kfc,
                                     KisSelectionSP selection)
    : KisSelectionBasedLayer(image, name, selection, kfc, true),
      m_d(new Private)
{
    connect(&m_d->updateSignalCompressor, SIGNAL(timeout()), SLOT(slotDelayedStaticUpdate()));
}

KisGeneratorLayer::KisGeneratorLayer(const KisGeneratorLayer& rhs)
    : KisSelectionBasedLayer(rhs),
      m_d(new Private)
{
    connect(&m_d->updateSignalCompressor, SIGNAL(timeout()), SLOT(slotDelayedStaticUpdate()));
}

KisGeneratorLayer::~KisGeneratorLayer()
{
}

void KisGeneratorLayer::setFilter(KisFilterConfigurationSP filterConfig)
{
    KisSelectionBasedLayer::setFilter(filterConfig);
    m_d->preparedRect = QRect();
    slotDelayedStaticUpdate();
}

void KisGeneratorLayer::slotDelayedStaticUpdate()
{
    /**
     * The mask might have been deleted from the layers stack in the
     * meanwhile. Just ignore the updates in the case.
     */

    KisLayerSP parentLayer(qobject_cast<KisLayer*>(parent().data()));
    if (!parentLayer) return;

    KisImageSP image = parentLayer->image();
    if (image) {
        image->addSpontaneousJob(new KisRecalculateGeneratorLayerJob(KisGeneratorLayerSP(this)));
    }
}

void KisGeneratorLayer::update()
{
    using KritaUtils::splitRectIntoPatches;
    using KritaUtils::optimalPatchSize;

    KisImageSP image = this->image().toStrongRef();
    const QRect updateRect = extent() | image->bounds();

    KisFilterConfigurationSP filterConfig = filter();
    KIS_SAFE_ASSERT_RECOVER_RETURN(filterConfig);

    if (filterConfig != m_d->preparedForFilter) {
        resetCache();
    }

    const QRegion processRegion(QRegion(updateRect) - m_d->preparedRect);
    if (processRegion.isEmpty()) return;

    KisGeneratorSP f = KisGeneratorRegistry::instance()->value(filterConfig->name());
    KIS_SAFE_ASSERT_RECOVER_RETURN(f);

    KisProcessingVisitor::ProgressHelper helper(this);

    KisPaintDeviceSP originalDevice = original();

    QVector<QRect> dirtyRegion;

    if (!m_d->strokeId.isNull()) {
        image->cancelStroke(m_d->strokeId);
        m_d->strokeId.clear();
    }

    KisGeneratorStrokeStrategy *stroke = new KisGeneratorStrokeStrategy(image);

    m_d->strokeId = image->startStroke(stroke);

    auto rc = processRegion.begin();
    while (rc != processRegion.end()) {
        QVector<QRect> tiles = splitRectIntoPatches(*rc, optimalPatchSize());

        Q_FOREACH (const QRect &tile, tiles) {
            KisStrokeJobData * jd = stroke->createJobData(this, f, originalDevice, tile, filterConfig, helper);
            image->addJob(m_d->strokeId, jd);
        }

        rc++;
    }

    image->endStroke(m_d->strokeId);

    m_d->preparedRect = updateRect;
    m_d->preparedForFilter = filterConfig;
}

bool KisGeneratorLayer::accept(KisNodeVisitor & v)
{
    return v.visit(this);
}

void KisGeneratorLayer::accept(KisProcessingVisitor &visitor, KisUndoAdapter *undoAdapter)
{
    return visitor.visit(this, undoAdapter);
}

QIcon KisGeneratorLayer::icon() const
{
    return KisIconUtils::loadIcon("fillLayer");
}

KisBaseNode::PropertyList KisGeneratorLayer::sectionModelProperties() const
{
    KisFilterConfigurationSP filterConfig = filter();

    KisBaseNode::PropertyList l = KisLayer::sectionModelProperties();
    l << KisBaseNode::Property(KoID("generator", i18n("Generator")),
                               KisGeneratorRegistry::instance()->value(filterConfig->name())->name());

    return l;
}

void KisGeneratorLayer::setX(qint32 x)
{
    KisSelectionBasedLayer::setX(x);
    m_d->preparedRect = QRect();
    m_d->updateSignalCompressor.start();
}

void KisGeneratorLayer::setY(qint32 y)
{
    KisSelectionBasedLayer::setY(y);
    m_d->preparedRect = QRect();
    m_d->updateSignalCompressor.start();
}

void KisGeneratorLayer::resetCache()
{
    KisSelectionBasedLayer::resetCache();
    m_d->preparedRect = QRect();
    m_d->updateSignalCompressor.start();
}

void KisGeneratorLayer::setDirty(const QVector<QRect> &rects)
{
    KisSelectionBasedLayer::setDirty(rects);
    m_d->updateSignalCompressor.start();
}

