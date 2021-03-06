/*  This file is part of the Kate project.
 *
 *  Copyright (C) 2013 Dominik Haumann <dhaumann.org>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Library General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Library General Public License for more details.
 *
 *  You should have received a copy of the GNU Library General Public License
 *  along with this library; see the file COPYING.LIB.  If not, write to
 *  the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 *  Boston, MA 02110-1301, USA.
 */

#ifndef KATE_PROJECT_VIEW_TREE_CONTEXT_MENU_H
#define KATE_PROJECT_VIEW_TREE_CONTEXT_MENU_H

#include <QString>
#include <QPoint>

class QWidget;

class KateProjectTreeViewContextMenu
{
  public:
    /**
     * construct project view for given project
     * @param pluginView our plugin view
     * @param project project this view is for
     */
    KateProjectTreeViewContextMenu ();

    /**
     * deconstruct project
     */
    ~KateProjectTreeViewContextMenu ();

    /**
     * our project.
     * @return project
     */
    void exec(const QString& filename, const QPoint& pos, QWidget* parent);

  protected:
};

#endif

// kate: space-indent on; indent-width 2; replace-tabs on;
