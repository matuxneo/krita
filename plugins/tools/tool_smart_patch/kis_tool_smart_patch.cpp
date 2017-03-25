/*
 *  Copyright (c) 2017 Eugene Ingerman
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

#include "kis_tool_smart_patch.h"

#include <klocalizedstring.h>
#include <QAction>
#include <QLabel>
#include <kactioncollection.h>

#include <KoCanvasBase.h>
#include <KoCanvasController.h>

#include <KisViewManager.h>
#include "kis_canvas2.h"
#include "kis_cursor.h"
#include "kis_config.h"
#include "kundo2magicstring.h"

#include "KoProperties.h"
#include "KoColorSpaceRegistry.h"

#include "kis_node_manager.h"

#include "kis_tool_smart_patch_options_widget.h"
#include "libs/image/kis_paint_device_debug_utils.h"

#include "kis_resources_snapshot.h"
#include "kis_layer.h"

#include "kis_inpaint_mask.h"

void patchImage(KisPaintDeviceSP imageDev, KisPaintDeviceSP maskDev, int radius, int accuracy);

struct KisToolSmartPatch::Private
{
    KisMaskSP mask = nullptr;
    KisNodeSP maskNode = nullptr;
    KisNodeSP paintNode = nullptr;
    KisPaintDeviceSP imageDev = nullptr;
    KisPaintDeviceSP maskDev = nullptr;
    KisResourcesSnapshotSP resources = nullptr;
    KoColor currentFgColor;
    KisToolSmartPatchOptionsWidget *optionsWidget = nullptr;
};


KisToolSmartPatch::KisToolSmartPatch(KoCanvasBase * canvas)
    : KisToolFreehand(canvas,
                      KisCursor::load("tool_freehand_cursor.png", 5, 5),
                      kundo2_i18n("Smart Patch Stroke")),
      m_d(new Private)
{
    setObjectName("tool_SmartPatch");
}

KisToolSmartPatch::~KisToolSmartPatch()
{
    m_d->optionsWidget = nullptr;
}

void KisToolSmartPatch::activate(ToolActivation activation, const QSet<KoShape*> &shapes)
{
    KisToolFreehand::activate(activation, shapes);
}

void KisToolSmartPatch::deactivate()
{
    KisToolFreehand::deactivate();
}

void KisToolSmartPatch::resetCursorStyle()
{
    KisToolFreehand::resetCursorStyle();
}


bool KisToolSmartPatch::canCreateInpaintMask() const
{
    KisNodeSP node = currentNode();
    return node && node->inherits("KisLayer");
}

void KisToolSmartPatch::inpaintImage(KisPaintDeviceSP maskDev, KisPaintDeviceSP imageDev)
{
    int accuracy = 0;
    int patchRadius = 2;

    if( !m_d.isNull() && m_d->optionsWidget ){
        accuracy = m_d->optionsWidget->getAccuracy();
        patchRadius = m_d->optionsWidget->getPatchRadius();
    }
    patchImage( imageDev, maskDev, patchRadius, accuracy );

}

void KisToolSmartPatch::activatePrimaryAction()
{
    KisToolFreehand::activatePrimaryAction();
}

void KisToolSmartPatch::deactivatePrimaryAction()
{
    KisToolFreehand::deactivatePrimaryAction();
}

void KisToolSmartPatch::createInpaintMask( void )
{
    m_d->mask = new KisInpaintMask();
    KisLayerSP parentLayer = qobject_cast<KisLayer*>(m_d->paintNode.data());
    m_d->mask->initSelection(parentLayer);
    image()->addNode( m_d->mask, m_d->paintNode );
}

void KisToolSmartPatch::deleteInpaintMask( void )
{
    KisLayerSP inpaintMaskLayer = qobject_cast<KisLayer*>( m_d->mask );
    image()->removeNode(inpaintMaskLayer);

    KisCanvas2 * kiscanvas = static_cast<KisCanvas2*>(canvas());
    KisViewManager* viewManager = kiscanvas->viewManager();

    if( ! m_d->paintNode.isNull() )
        viewManager->nodeManager()->slotNonUiActivatedNode( m_d->paintNode );

    m_d->paintNode = nullptr;
    m_d->mask = nullptr;
}

void KisToolSmartPatch::beginPrimaryAction(KoPointerEvent *event)
{
    KisNodeSP node = currentNode();
    if (!node) return;
    KisCanvas2 * kiscanvas = static_cast<KisCanvas2*>(canvas());
    KisViewManager* viewManager = kiscanvas->viewManager();

    if (!m_d->mask.isNull()) {
        viewManager->nodeManager()->slotNonUiActivatedNode(m_d->mask);
    } else {
        m_d->paintNode = viewManager->nodeManager()->activeNode();
        m_d->imageDev = m_d->paintNode->paintDevice();

        createInpaintMask();
        viewManager->nodeManager()->slotNonUiActivatedNode(m_d->mask);

        if ( ! m_d->mask.isNull() ){
            m_d->mask->setProperty("visible", false);
            m_d->mask->setProperty("temporary", true);
            m_d->mask->setProperty("inpaintmask", true);
        }
        KisToolFreehand::beginPrimaryAction(event);

        m_d->currentFgColor = canvas()->resourceManager()->foregroundColor();
        canvas()->resourceManager()->setForegroundColor(KoColor(Qt::black, KoColorSpaceRegistry::instance()->alpha8()));
    }
}

void KisToolSmartPatch::continuePrimaryAction(KoPointerEvent *event)
{
    KisToolFreehand::continuePrimaryAction(event);
}


void KisToolSmartPatch::endPrimaryAction(KoPointerEvent *event)
{
    if( mode() != KisTool::PAINT_MODE )
        return;

    KisToolFreehand::endPrimaryAction(event);

    //Next line is important. We need to wait for the paint operation to finish otherwise
    //mask will be incomplete.
    image()->waitForDone();

    m_d->maskDev = new KisPaintDevice(KoColorSpaceRegistry::instance()->alpha8());
    m_d->maskDev->makeCloneFrom(currentNode()->paintDevice(), currentNode()->paintDevice()->extent());

    inpaintImage( m_d->maskDev, m_d->imageDev );

//    KIS_DUMP_DEVICE_2(m_d->imageDev, m_d->imageDev->extent(), "patched", "/home/eugening/Projects/Out");
//    KIS_DUMP_DEVICE_2(m_d->maskDev, m_d->imageDev->extent(), "output", "/home/eugening/Projects/Out");

    deleteInpaintMask();
    canvas()->resourceManager()->setForegroundColor(m_d->currentFgColor);
}

QWidget * KisToolSmartPatch::createOptionWidget()
{
    KisCanvas2 * kiscanvas = dynamic_cast<KisCanvas2*>(canvas());

    m_d->optionsWidget = new KisToolSmartPatchOptionsWidget(kiscanvas->viewManager()->resourceProvider(), 0);
    m_d->optionsWidget->setObjectName(toolId() + "option widget");

    return m_d->optionsWidget;
}


