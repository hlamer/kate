/* This file is part of the KDE libraries
   Copyright (C) 2001 Christoph Cullmann <cullmann@kde.org>
   Copyright (C) 2001 Joseph Wenninger <jowenn@kde.org>
   Copyright (C) 1999 Jochen Wilhelmy <digisnap@cs.tu-berlin.de>

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Library General Public
   License version 2 as published by the Free Software Foundation.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Library General Public License for more details.

   You should have received a copy of the GNU Library General Public License
   along with this library; see the file COPYING.LIB.  If not, write to
   the Free Software Foundation, Inc., 59 Temple Place - Suite 330,
   Boston, MA 02111-1307, USA.
*/

// $Id$

//BEGIN includes
#include "katedocument.h"
#include "katedocument.moc"

#include <ktexteditor/plugin.h>

#include "katefactory.h"
#include "kateviewdialog.h"
#include "katedialogs.h"
#include "katebuffer.h"
#include "katehighlight.h"
#include "kateview.h"
#include "kateviewinternal.h"
#include "katetextline.h"
#include "katecmd.h"
#include "kateexportaction.h"
#include "katecodefoldinghelpers.h"
#include "kateundo.h"

#include <qfileinfo.h>
#include <qfile.h>
#include <qfocusdata.h>
#include <qpainter.h>
#include <qpixmap.h>
#include <qevent.h>
#include <qpaintdevicemetrics.h>
#include <qiodevice.h>
#include <qclipboard.h>
#include <qregexp.h>
#include <qtimer.h>
#include <qobject.h>
#include <qapplication.h>
#include <qclipboard.h>
#include <qpainter.h>
#include <qfile.h>
#include <qtextstream.h>
#include <qtextcodec.h>
#include <qptrstack.h>

#include <kmessagebox.h>
#include <klocale.h>
#include <kconfig.h>
#include <kglobal.h>
#include <kurldrag.h>
#include <kprinter.h>
#include <kapp.h>
#include <kpopupmenu.h>
#include <klineeditdlg.h>
#include <kconfig.h>
#include <kcursor.h>
#include <kcharsets.h>
#include <kfiledialog.h>
#include <kmessagebox.h>
#include <kstringhandler.h>
#include <kaction.h>
#include <kstdaction.h>
#include <kparts/event.h>
#include <kiconloader.h>
#include <kxmlguifactory.h>
#include <dcopclient.h>
#include <kwin.h>
#include <kdialogbase.h>
#include <kdebug.h>
#include <kinstance.h>
#include <kglobalsettings.h>
#include <ksavefile.h>
#include <klibloader.h>

#include "kateviewhighlightaction.h"

//END  includes

//
// KateDocument Constructor
//
KateDocument::KateDocument(bool bSingleViewMode, bool bBrowserView, bool bReadOnly,
                                           QWidget *parentWidget, const char *widgetName,
                                           QObject *, const char *)
  : Kate::Document (), viewFont(), printFont(), hlManager(HlManager::self ())
{
  KateFactory::registerDocument (this);

  setMarksUserChangable(markType01);

  regionTree=new KateCodeFoldingTree(this);
  myActiveView = 0L;

  hlSetByUser = false;
  setInstance( KateFactory::instance() );

  editSessionNumber = 0;
  editIsRunning = false;
  noViewUpdates = false;
  editCurrentUndo = 0L;
  editWithUndo = false;

  blockSelect = false;
  restoreMarks = false;

  m_bSingleViewMode = bSingleViewMode;
  m_bBrowserView = bBrowserView;
  m_bReadOnly = bReadOnly;

  myMarks.setAutoDelete (true);

  selectStart.line = -1;
  selectStart.col = -1;
  selectEnd.line = -1;
  selectEnd.col = -1;
  selectAnchor.line = -1;
  selectAnchor.col = -1;

  readOnly = false;
  newDoc = false;

  modified = false;
  m_highlightedEnd = 0;

  // some defaults
  _configFlags = KateDocument::cfAutoIndent | KateDocument::cfTabIndents | KateDocument::cfKeepIndentProfile
    | KateDocument::cfRemoveSpaces
    | KateDocument::cfDelOnInput | KateDocument::cfWrapCursor
    | KateDocument::cfShowTabs | KateDocument::cfSmartHome;

  myEncoding = QString::fromLatin1(QTextCodec::codecForLocale()->name());

  setFont (ViewFont,KGlobalSettings::fixedFont());
  setFont (PrintFont,KGlobalSettings::fixedFont());

  myDocName = QString ("");
  fileInfo = new QFileInfo ();

  myCmd = new KateCmd (this);

  connect(this,SIGNAL(modifiedChanged ()),this,SLOT(slotModChanged ()));

  buffer = new KateBuffer (this);

  connect(buffer, SIGNAL(loadingFinished()), this, SLOT(slotLoadingFinished()));

  connect(buffer, SIGNAL(linesChanged(int)), this, SLOT(slotBufferChanged()));

  connect(buffer, SIGNAL(tagLines(int,int)), this, SLOT(tagLines(int,int)));
  connect(buffer, SIGNAL(pleaseHighlight(uint,uint)),this,SLOT(slotBufferUpdateHighlight(uint,uint)));

  connect(buffer,SIGNAL(foldingUpdate(unsigned int , QMemArray<signed char>*,bool*,bool)),regionTree,SLOT(updateLine(unsigned int, QMemArray<signed char>*,bool *,bool)));
  connect(regionTree,SIGNAL(setLineVisible(unsigned int, bool)), buffer,SLOT(setLineVisible(unsigned int,bool)));
  connect(buffer,SIGNAL(codeFoldingUpdated()),this,SIGNAL(codeFoldingUpdated()));
  m_highlightTimer = new QTimer(this);
  connect(m_highlightTimer, SIGNAL(timeout()), this, SLOT(slotBufferUpdateHighlight()));


  colors[0] = KGlobalSettings::baseColor();
  colors[1] = KGlobalSettings::highlightColor();
  colors[2] = KGlobalSettings::baseColor();  // out of order alternateBackgroundColor();

  m_highlight = 0L;
  tabChars = 8;

  clear();

  // if the user changes the highlight with the dialog, notify the doc
  connect(hlManager,SIGNAL(changed()),SLOT(internalHlChanged()));

  readConfig();

  if ( m_bSingleViewMode )
  {
    KTextEditor::View *view = createView( parentWidget, widgetName );
    view->show();
    setWidget( view );
  }
  
  KTrader::OfferList::Iterator it(KateFactory::plugins()->begin());
  for( ; it != KateFactory::plugins()->end(); ++it)
  {
    KService::Ptr ptr = (*it);

    KLibFactory *factory = KLibLoader::self()->factory( QFile::encodeName(ptr->library()) );
    if (factory)
    {
      KTextEditor::Plugin *plugin = static_cast<KTextEditor::Plugin *>(factory->create(this, ptr->name().latin1(), "KTextEditor::Plugin"));
      plugin->setDocument (this);
    }
  }
}

//
// KateDocument Destructor
//
KateDocument::~KateDocument()
{
  if ( !m_bSingleViewMode )
  {
    myViews.setAutoDelete( true );
    myViews.clear();
    myViews.setAutoDelete( false );
  }

  m_highlight->release();
  myMarks.clear ();
  
  KateFactory::deregisterDocument (this);
}

//
// KTextEditor::Document stuff
//

KTextEditor::View *KateDocument::createView( QWidget *parent, const char *name )
{
  return new KateView( this, parent, name);
}

QPtrList<KTextEditor::View> KateDocument::views () const
{
  return _views;
};

//
// KTextEditor::EditInterface stuff
//

QString KateDocument::text() const
{
  QString s;

  for (uint i=0; i < buffer->count(); i++)
  {
    TextLine::Ptr textLine = buffer->line(i);
    s.append (textLine->string());
    if ( (i < (buffer->count()-1)) )
      s.append('\n');
  }

  return s;
}

QString KateDocument::text ( uint startLine, uint startCol, uint endLine, uint endCol ) const
{
  QString s;

  for (uint i=startLine; (i <= endLine) && (i < buffer->count()); i++)
  {
    TextLine::Ptr textLine = buffer->line(i);

    if (i == startLine)
      s.append(textLine->string().mid (startCol, textLine->length()-startCol));
    else if (i == endLine)
      s.append(textLine->string().mid (0, endCol));
    else
      s.append(textLine->string());

    if ( i < endLine )
      s.append('\n');
  }

  return s;
}

QString KateDocument::textLine( uint line ) const
{
  return buffer->plainLine(line);
}

bool KateDocument::setText(const QString &s)
{
  clear();
  return insertText (0, 0, s);
}

bool KateDocument::clear()
{
  KateTextCursor cursor;
  KateView *view;

  cursor.col = cursor.line = 0;
  for (view = myViews.first(); view != 0L; view = myViews.next() ) {
    view->myViewInternal->clear();
    view->myViewInternal->tagAll();
  }

  eolMode = KateDocument::eolUnix;

  buffer->clear();
  clearMarks ();

  clearUndo();
  clearRedo();

  setModified(false);

  internalSetHlMode(0); //calls updateFontData()

  return true;
}

bool KateDocument::doInsertText( uint line, uint col, const QString &s )
{
  if (s.isEmpty())
    return true;

  uint insertPos = col;
  uint len = s.length();
  QChar ch;
  QString buf;
  uint endLine, endCol, startLine, startCol;

  startLine = line;
  startCol = col;
  endLine = line;
  endCol = col;

  for (uint pos = 0; pos < len; pos++)
  {
    ch = s[pos];

    if (ch == '\n')
    {
      editInsertText (line, insertPos, buf);
      editWrapLine (line, insertPos + buf.length());

      line++;
      endLine++;
      insertPos = 0;
      buf.truncate(0);
    }
    else
      buf += ch; // append char to buffer
  }

  editInsertText (line, insertPos, buf);

  return true;
}

bool KateDocument::insertText( uint line, uint col, const QString &s )
{
  if (s.isEmpty())
    return true;

  editStart ();

  bool result = doInsertText(line, col, s);

  editEnd ();

  return result;
}

bool KateDocument::doRemoveText ( uint startLine, uint startCol,
				  uint endLine, uint endCol )
{
  TextLine::Ptr l, tl;
  uint deletePos = 0;
  uint endPos = 0;
  uint line = 0;

  l = buffer->line(startLine);

  if (!l)
    return false;

  if (startLine == endLine)
  {
    editRemoveText (startLine, startCol, endCol-startCol);
  }
  else if ((startLine+1) == endLine)
  {
    editRemoveText (startLine, startCol, l->length()-startCol);
    editRemoveText (startLine+1, 0, endCol);
    editUnWrapLine (startLine, startCol);
  }
  else
  {
    for (line = startLine; line <= endLine; line++)
    {
      if ((line > startLine) && (line < endLine))
      {
        deletePos = 0;

        editRemoveText (startLine, deletePos, l->length()-startCol);
        editUnWrapLine (startLine, deletePos);
      }
      else
      {
        if (line == startLine)
        {
          deletePos = startCol;
          endPos = l->length();
        }
         else
        {
          deletePos = 0;
          endPos = endCol;
        }

        l->replace (deletePos, endPos-deletePos, 0, 0);
        editRemoveText (startLine, deletePos, endPos-deletePos);
      }
    }
  }
  
  return true;
}

bool KateDocument::removeText ( uint startLine, uint startCol,
				uint endLine, uint endCol )
{
  if (!buffer->line(startLine))
    return false;

  editStart ();

  doRemoveText(startLine, startCol, endLine, endCol);

  editEnd ();

  return true;
}

bool KateDocument::insertLine( uint l, const QString &str )
{
  if (l > buffer->count())
    return false;

  editStart ();

  editInsertLine (l, str);

  editEnd ();

  return true;
}

bool KateDocument::removeLine( uint line )
{
  editStart ();
  
  bool end = editRemoveLine (line);

  editEnd ();

  return end;
}

uint KateDocument::length() const
{
  return text().length();
}

uint KateDocument::numLines() const
{
  return buffer->count();
}

uint KateDocument::numVisLines() const
{
  return buffer->count()-regionTree->getHiddenLinesCount();
}

int KateDocument::lineLength ( uint line ) const
{
  return textLength(line);
}

//
// KTextEditor::EditInterface internal stuff
//

//
// Starts an edit session with (or without) undo, update of view disabled during session
//
void KateDocument::editStart (bool withUndo)
{
  editSessionNumber++;

  if (editSessionNumber > 1)
    return;

  editIsRunning = true;
  noViewUpdates = true;
  editWithUndo = withUndo;
  buffer->setAllowHlUpdate(false);

  editTagLineStart = 0xffffff;
  editTagLineEnd = 0;

  if (editWithUndo)
  {
    if (undoItems.count () > myUndoSteps)
      undoItems.removeFirst ();
    editCurrentUndo = new KateUndoGroup (this);
  }
  else
    editCurrentUndo = 0L;

  for (uint z = 0; z < myViews.count(); z++)
  {
    KateView *v = myViews.at(z);
    v->myViewInternal->cursorCacheChanged = false;
    v->myViewInternal->tagLinesFrom = -1;
    v->myViewInternal->cursorCache = v->myViewInternal->getCursor();
  }
}

//
// End edit session and update Views
//
void KateDocument::editEnd ()
{
  if (editSessionNumber == 0)
    return;
   
  // wrap the new/changed text
  if (editSessionNumber == 1)
    if (myWordWrap)
      wrapText (editTagLineStart, editTagLineEnd, myWordWrapAt);

  editSessionNumber--;

  if (editSessionNumber > 0)
    return;

  buffer->setAllowHlUpdate(true);

  if (editTagLineStart <= editTagLineEnd)
    updateLines(editTagLineStart, editTagLineEnd);

  if (editWithUndo && editCurrentUndo)
  {
    undoItems.append (editCurrentUndo);
    editCurrentUndo = 0L;
    emit undoChanged ();
  }

  for (uint z = 0; z < myViews.count(); z++)
  {
    KateView *v = myViews.at(z);

    if (v->myViewInternal->tagLinesFrom > -1)
    {
      int startTagging = QMIN( v->myViewInternal->tagLinesFrom, (int)editTagLineStart );
      int endTagging = getRealLine( v->myViewInternal->lastLine() );
      v->myViewInternal->tagRealLines (startTagging, endTagging);
    }
    else
      v->myViewInternal->tagRealLines (editTagLineStart, editTagLineEnd);

    if (v->myViewInternal->cursorCacheChanged)
      v->myViewInternal->updateCursor( v->myViewInternal->cursorCache );
    v->myViewInternal->updateView();

    v->myViewInternal->tagLinesFrom = -1;
    v->myViewInternal->cursorCacheChanged = false;
  }

  setModified(true);
  emit textChanged ();

  noViewUpdates = false;
  editIsRunning = false;
}

bool KateDocument::wrapText (uint startLine, uint endLine, uint col)
{
  if (endLine < startLine)
    return false;

  if (col == 0)
    return false;

  editStart ();

  uint line = startLine;
  int z = 0;

  while(line <= endLine)
  {
    TextLine::Ptr l = buffer->line(line);

    if (l->length() > col)
    {
      const QChar *text = l->text();

      for (z=col; z>0; z--)
      {
        if (z < 1) break;
        if (text[z].isSpace()) break;
      }

      if (!(z < 1))
      {
        z++; // (anders: avoid the space at the beginning of the line)
        editWrapLine (line, z);
        endLine++;
      }
    }

    line++;

    if (line >= numLines()) break;
  };

  editEnd ();

  return true;
}

void KateDocument::editAddUndo (KateUndo *undo)
{
  if (!undo)
    return;

  if (editIsRunning && editWithUndo && editCurrentUndo)
    editCurrentUndo->addItem (undo);
  else
    delete undo;
}

void KateDocument::editTagLine (uint line)
{
  if (line < editTagLineStart)
    editTagLineStart = line;

  if (line > editTagLineEnd)
    editTagLineEnd = line;
}

void KateDocument::editInsertTagLine (uint line)
{
  if (line <= editTagLineStart)
    editTagLineStart++;

  if (line <= editTagLineEnd)
    editTagLineEnd++;
}

void KateDocument::editRemoveTagLine (uint line)
{
  if ((line < editTagLineStart) && (editTagLineStart > 0))
    editTagLineStart--;

  if ((line < editTagLineEnd) && (editTagLineEnd > 0))
    editTagLineEnd--;
}

bool KateDocument::editInsertText ( uint line, uint col, const QString &s )
{
  TextLine::Ptr l;

  l = buffer->line(line);

  if (!l)
    return false;

  editStart ();

  editAddUndo (new KateUndo (this, KateUndo::editInsertText, line, col, s.length(), s));

  l->replace(col, 0, s.unicode(), s.length());

  buffer->changeLine(line);
  editTagLine (line);

  editEnd ();

  return true;
}

bool KateDocument::editRemoveText ( uint line, uint col, uint len )
{
  TextLine::Ptr l;
  uint cLine, cCol;

  l = buffer->line(line);

  if (!l)
    return false;

  editStart ();

  editAddUndo (new KateUndo (this, KateUndo::editRemoveText, line, col, len, l->string().mid(col, len)));

  l->replace(col, len, 0L, 0);

  buffer->changeLine(line);

  editTagLine(line);

  for (uint z = 0; z < myViews.count(); z++)
  {
    KateView *v = myViews.at(z);

    cLine = v->myViewInternal->cursorCache.line;
    cCol = v->myViewInternal->cursorCache.col;

    if ( (cLine == line) && (cCol > col) )
    {
      if ((cCol - len) >= col)
      {
        if ((cCol - len) > 0)
          cCol = cCol-len;
        else
          cCol = 0;
      }
      else
        cCol = col;

      v->myViewInternal->cursorCache.line = line;
      v->myViewInternal->cursorCache.col = cCol;
      v->myViewInternal->cursorCacheChanged = true;
    }
  }

  editEnd ();

  return true;
}

bool KateDocument::editWrapLine ( uint line, uint col )
{
  TextLine::Ptr l, tl;
  KateView *view;

  l = buffer->line(line);
  tl = new TextLine();

  if (!l || !tl)
    return false;

  editStart ();

  editAddUndo (new KateUndo (this, KateUndo::editWrapLine, line, col, 0, 0));

  l->wrap (tl, col);

  buffer->insertLine (line+1, tl);
  buffer->changeLine(line);

  if (!myMarks.isEmpty())
  {
    bool b = false;

    for (uint z=0; z<myMarks.count(); z++)
    {
      if (myMarks.at(z)->line > line+1)
      {
        myMarks.at(z)->line = myMarks.at(z)->line+1;
        b = true;
      }
    }

    if (b)
      emit marksChanged ();
  }

  editInsertTagLine (line);
  editTagLine(line);
  editTagLine(line+1);

  regionTree->lineHasBeenInserted(line); //test line or line +1

  for (uint z2 = 0; z2 < myViews.count(); z2++)
  {
    view = myViews.at(z2);

     if (line >= getRealLine( view->myViewInternal->firstLine() ) )
     {
       if ((view->myViewInternal->tagLinesFrom > (int)line) || (view->myViewInternal->tagLinesFrom == -1))
         view->myViewInternal->tagLinesFrom = line;
     }
//     else
//       view->myViewInternal->newStartLineReal++;

    // correct cursor position
    if (view->myViewInternal->cursorCache.line > (int)line)
    {
      view->myViewInternal->cursorCache.line++;
      view->myViewInternal->cursorCacheChanged = true;
    }
    else if ( view->myViewInternal->cursorCache.line == (int)line
              && view->myViewInternal->cursorCache.col >= (int)col )
    {
      view->myViewInternal->cursorCache.col = tl->length();
      view->myViewInternal->cursorCache.line++;
      view->myViewInternal->cursorCacheChanged = true;
    }

  }
  editEnd ();
  return true;
}

bool KateDocument::editUnWrapLine ( uint line, uint col )
{
  TextLine::Ptr l, tl;
  KateView *view;
  uint cLine, cCol;

  l = buffer->line(line);
  tl = buffer->line(line+1);

  if (!l || !tl)
    return false;

  editStart ();

  editAddUndo (new KateUndo (this, KateUndo::editUnWrapLine, line, col, 0, 0));

  l->unWrap (col, tl, tl->length());
  l->setContext (tl->ctx(), tl->ctxLength());

  buffer->changeLine(line);
  buffer->removeLine(line+1);

  if (!myMarks.isEmpty())
  {
    bool b = false;

    for (uint z=0; z<myMarks.count(); z++)
    {
      if (myMarks.at(z)->line > line)
      {
        if (myMarks.at(z)->line == line+1)
          myMarks.remove(z);
        else
          myMarks.at(z)->line = myMarks.at(z)->line-1;
        b = true;
      }
    }

    if (b)
      emit marksChanged ();
  }

  editRemoveTagLine (line);
  editTagLine(line);
  editTagLine(line+1);

  for (uint z2 = 0; z2 < myViews.count(); z2++)
  {
    view = myViews.at(z2);
    
     if (line >= getRealLine( view->myViewInternal->firstLine() ) )
     {
       if ((view->myViewInternal->tagLinesFrom > (int)line) || (view->myViewInternal->tagLinesFrom == -1))
         view->myViewInternal->tagLinesFrom = line;
     }
//     else
//       view->myViewInternal->newStartLineReal--;

    cLine = view->myViewInternal->cursorCache.line;
    cCol = view->myViewInternal->cursorCache.col;

    if ( (cLine == (line+1)) || ((cLine == line) && (cCol >= col)) )
    {
      cCol = col;

      view->myViewInternal->cursorCache.line = line;
      view->myViewInternal->cursorCache.col = cCol;
      view->myViewInternal->cursorCacheChanged = true;
    }
  }

  editEnd ();

  return true;
}

bool KateDocument::editInsertLine ( uint line, const QString &s )
{
  KateView *view;

  editStart ();

  editAddUndo (new KateUndo (this, KateUndo::editInsertLine, line, 0, s.length(), s));

  TextLine::Ptr TL=new TextLine();
  TL->append(s.unicode(),s.length());
  buffer->insertLine(line,TL);
  buffer->changeLine(line);

  editInsertTagLine (line);
  editTagLine(line);


  if (!myMarks.isEmpty())
  {
    bool b = false;

    for (uint z=0; z<myMarks.count(); z++)
    {
       if (myMarks.at(z)->line >= line)
      {
        myMarks.at(z)->line = myMarks.at(z)->line+1;
        b = true;
      }
    }

    if (b)
      emit marksChanged ();
  }

  regionTree->lineHasBeenInserted(line);

  for (uint z2 = 0; z2 < myViews.count(); z2++)
  {
    view = myViews.at(z2);
    
     if (line >= getRealLine( view->myViewInternal->firstLine() ) )
     {
       if ((view->myViewInternal->tagLinesFrom > (int)line) || (view->myViewInternal->tagLinesFrom == -1))
         view->myViewInternal->tagLinesFrom = line;
     }
//     else
//       view->myViewInternal->newStartLineReal++;
  }

  editEnd ();

  return true;
}

bool KateDocument::editRemoveLine ( uint line )
{
  KateView *view;
  uint cLine, cCol;

//  regionTree->lineHasBeenRemoved(line);// is this the right place ?

  if (numLines() == 1)
    return false;

  editStart ();

  editAddUndo (new KateUndo (this, KateUndo::editRemoveLine, line, 0, textLength(line), textLine(line) ));

  buffer->removeLine(line);

  editRemoveTagLine (line);

  if (!myMarks.isEmpty())
  {
    bool b = false;

    for (uint z=0; z<myMarks.count(); z++)
    {
      if (myMarks.at(z)->line >= line)
      {
        if (myMarks.at(z)->line == line)
          myMarks.remove(z);
        else
          myMarks.at(z)->line = myMarks.at(z)->line-1;
        b = true;
      }
    }

    if (b)
      emit marksChanged ();
  }

  regionTree->lineHasBeenRemoved(line);

  for (uint z2 = 0; z2 < myViews.count(); z2++)
  {
    view = myViews.at(z2);

     if (line >= getRealLine( view->myViewInternal->firstLine() ) )
     {
       if ((view->myViewInternal->tagLinesFrom > (int)line) || (view->myViewInternal->tagLinesFrom == -1))
         view->myViewInternal->tagLinesFrom = line;
     }
//     else
//       view->myViewInternal->newStartLineReal--;

    cLine = view->myViewInternal->cursorCache.line;
    cCol = view->myViewInternal->cursorCache.col;

    if ( (cLine == line) )
    {
      if (line < lastLine())
        view->myViewInternal->cursorCache.line = line;
      else
        view->myViewInternal->cursorCache.line = line-1;

      cCol = 0;
      view->myViewInternal->cursorCache.col = cCol;
      view->myViewInternal->cursorCacheChanged = true;
    }
  }

  editEnd();

  return true;
}

//
// KTextEditor::SelectionInterface stuff
//

bool KateDocument::setSelection ( uint startLine, uint startCol, uint endLine, uint endCol )
{
  if( hasSelection() )
    tagLines( selectStart.line, selectEnd.line );

  if (startLine < endLine)
  {
    selectStart.line = startLine;
    selectStart.col = startCol;
    selectEnd.line = endLine;
    selectEnd.col = endCol;
  }
  else if (startLine > endLine)
  {
    selectStart.line = endLine;
    selectStart.col = endCol;
    selectEnd.line = startLine;
    selectEnd.col = startCol;
  }
  else if (startCol < endCol)
  {
    selectStart.line = startLine;
    selectStart.col = startCol;
    selectEnd.line = endLine;
    selectEnd.col = endCol;
  }
  else if (startCol >= endCol)
  {
    selectStart.line = endLine;
    selectStart.col = endCol;
    selectEnd.line = startLine;
    selectEnd.col = startCol;
  }

  if( hasSelection() )
    tagLines( selectStart.line, selectEnd.line );

  emit selectionChanged ();

  return true;
}

bool KateDocument::clearSelection ()
{
  if( !hasSelection() )
    return false;

  tagLines(selectStart.line,selectEnd.line);

  selectStart.line = -1;
  selectStart.col = -1;
  selectEnd.line = -1;
  selectEnd.col = -1;
  selectAnchor.line = -1;
  selectAnchor.col = -1;

  emit selectionChanged();

  return true;
}

bool KateDocument::hasSelection() const
{
  return ((selectStart.col != selectEnd.col) || (selectEnd.line != selectStart.line));
}

QString KateDocument::selection() const
{
  QString s;

  for (int z=selectStart.line; z <= selectEnd.line; z++)
  {
      QString line = textLine(z);

      if (!blockSelect)
      {
        if ((z > selectStart.line) && (z < selectEnd.line))
          s.append (line);
        else
        {
          if ((z == selectStart.line) && (z == selectEnd.line))
            s.append (line.mid(selectStart.col, selectEnd.col-selectStart.col));
          else if ((z == selectStart.line))
            s.append (line.mid(selectStart.col, line.length()-selectStart.col));
          else if ((z == selectEnd.line))
            s.append (line.mid(0, selectEnd.col));
        }
      }
      else
      {
        s.append (line.mid(selectStart.col, selectEnd.col-selectStart.col));
      }

      if (z < selectEnd.line)
        s.append (QChar('\n'));
    }

  return s;
}

bool KateDocument::removeSelectedText ()
{
  TextLine::Ptr textLine = 0L;
  int delLen, delStart, delLine;

  if (!hasSelection())
    return false;

  editStart ();

  for (uint z = 0; z < myViews.count(); z++)
  {
    KateView *v = myViews.at(z);
    if ((selectStart.line <= v->myViewInternal->cursorCache.line) && (v->myViewInternal->cursorCache.line<= selectEnd.line))
    {
      v->myViewInternal->cursorCache.line = selectStart.line;
      v->myViewInternal->cursorCache.col = selectStart.col;
      v->myViewInternal->cursorCacheChanged = true;
    }
  }

  int sl = selectStart.line;
  int el = selectEnd.line;
  int sc = selectStart.col;
  int ec = selectEnd.col;

  for (int z=el; z >= sl; z--)
  {
    textLine = buffer->line(z);
    if (!textLine)
      break;

    delLine = 0;
    delStart = 0;
    delLen = 0;

    if (!blockSelect)
    {
      if ((z > sl) && (z < el))
        delLine = 1;
      else
      {
        if ((z == sl) && (z == el))
        {
          delStart = sc;
          delLen = ec-sc;
        }
        else if ((z == sl))
        {
          delStart = sc;
          delLen = textLine->length()-sc;

          if (sl < el)
            delLen++;
        }
        else if ((z == el))
        {
          delStart = 0;
          delLen = ec;
        }
      }
    }
    else
    {
      delStart = sc;
      delLen = ec-sc;

      if (delStart >= (int)textLine->length())
      {
        delStart = 0;
        delLen = 0;
      }
      else if (delLen+delStart > (int)textLine->length())
        delLen = textLine->length()-delStart;
    }

    if (delLine == 1)
      editRemoveLine (z);
    else if (delStart+delLen > (int)textLine->length())
    {
      editRemoveText (z, delStart, textLine->length()-delStart);
      editUnWrapLine (z, delStart);
    }
    else
      editRemoveText (z, delStart, delLen);
  }

  clearSelection();

  editEnd ();

  return true;
}

bool KateDocument::selectAll()
{
  return setSelection (0, 0, lastLine(), textLength(lastLine()));
}

//
// KTextEditor::BlockSelectionInterface stuff
//

bool KateDocument::blockSelectionMode ()
{
  return blockSelect;
}

bool KateDocument::setBlockSelectionMode (bool on)
{
  if (on != blockSelect)
  {
    blockSelect = on;
    setSelection (selectStart.line, selectStart.col, selectEnd.line, selectEnd.col);
    KTextEditor::View *view;
    for (view = myViews.first(); view != 0L; view = myViews.next() ) {
      emit static_cast<KateView *>( view )->newStatus();
    }
  }

  return true;
}

bool KateDocument::toggleBlockSelectionMode ()
{
  setBlockSelectionMode (!blockSelect);
  return true;
}

//
// KTextEditor::UndoInterface stuff
//

uint KateDocument::undoCount () const
{
  return undoItems.count ();
}

uint KateDocument::redoCount () const
{
  return redoItems.count ();
}

uint KateDocument::undoSteps () const
{
  return myUndoSteps;
}

void KateDocument::setUndoSteps(uint steps)
{
  myUndoSteps = steps;

  emit undoChanged ();
}

void KateDocument::undo()
{
  if ((undoItems.count() >0) && undoItems.last())
  {
    undoItems.last()->undo();
    redoItems.append (undoItems.last());
    undoItems.removeLast ();

    emit undoChanged ();
  }
}

void KateDocument::redo()
{
  if ((redoItems.count() >0) && redoItems.last())
  {
    redoItems.last()->redo();
    undoItems.append (redoItems.last());
    redoItems.removeLast ();

    emit undoChanged ();
  }
}

void KateDocument::clearUndo()
{
  undoItems.setAutoDelete (true);
  undoItems.clear ();
  undoItems.setAutoDelete (false);

  emit undoChanged ();
}

void KateDocument::clearRedo()
{
  redoItems.setAutoDelete (true);
  redoItems.clear ();
  redoItems.setAutoDelete (false);

  emit undoChanged ();
}

QPtrList<KTextEditor::Cursor> KateDocument::cursors () const
{
  return myCursors;
}

//
// KTextEditor::SearchInterface stuff
//

bool KateDocument::searchText (unsigned int startLine, unsigned int startCol, const QString &text, unsigned int *foundAtLine, unsigned int *foundAtCol, unsigned int *matchLen, bool casesensitive, bool backwards)
{
  int line, col;
  int searchEnd;
  TextLine::Ptr textLine;
  uint foundAt, myMatchLen;
  bool found;                
  
  Q_ASSERT( startLine < numLines() );
  
  if (text.isEmpty())
    return false;

  line = startLine;
  col = startCol;

  if (!backwards)
  {
    searchEnd = lastLine();

    while (line <= searchEnd)
    {
      textLine = buffer->line(line);

      found = false;
      found = textLine->searchText (col, text, &foundAt, &myMatchLen, casesensitive, false);

      if (found)
      {
        (*foundAtLine) = line;
        (*foundAtCol) = foundAt;
        (*matchLen) = myMatchLen;
        return true;
      }

      col = 0;
      line++;
    }
  }
  else
  {
    // backward search
    searchEnd = 0;

    while (line >= searchEnd)
    {
      textLine = buffer->line(line);

      found = false;
      found = textLine->searchText (col, text, &foundAt, &myMatchLen, casesensitive, true);

        if (found)
      {
        (*foundAtLine) = line;
        (*foundAtCol) = foundAt;
        (*matchLen) = myMatchLen;
        return true;
      }

      if (line >= 1)
        col = textLength(line-1);

      line--;
    }
  }

  return false;
}

bool KateDocument::searchText (unsigned int startLine, unsigned int startCol, const QRegExp &regexp, unsigned int *foundAtLine, unsigned int *foundAtCol, unsigned int *matchLen, bool backwards)
{
  int line, col;
  int searchEnd;
  TextLine::Ptr textLine;
  uint foundAt, myMatchLen;
  bool found;

  if (regexp.isEmpty() || !regexp.isValid())
    return false;

  line = startLine;
  col = startCol;

  if (!backwards)
  {
    searchEnd = lastLine();

    while (line <= searchEnd)
    {
      textLine = buffer->line(line);

      found = false;
      found = textLine->searchText (col, regexp, &foundAt, &myMatchLen, false);

      if (found)
      {
        (*foundAtLine) = line;
        (*foundAtCol) = foundAt;
        (*matchLen) = myMatchLen;
        return true;
      }

      col = 0;
      line++;
    }
  }
  else
  {
    // backward search
    searchEnd = 0;

    while (line >= searchEnd)
    {
      textLine = buffer->line(line);

      found = false;
      found = textLine->searchText (col, regexp, &foundAt, &myMatchLen, true);

        if (found)
      {
        (*foundAtLine) = line;
        (*foundAtCol) = foundAt;
        (*matchLen) = myMatchLen;
        return true;
      }

      if (line >= 1)
        col = textLength(line-1);

      line--;
    }
  }

  return false;
}

//
// KTextEditor::HighlightingInterface stuff
//

uint KateDocument::hlMode ()
{
  return hlManager->findHl(m_highlight);
}

bool KateDocument::setHlMode (uint mode)
{
  if (internalSetHlMode (mode))
  {
    setDontChangeHlOnSave();
    return true;
  }

  return false;
}

bool KateDocument::internalSetHlMode (uint mode)
{
  Highlight *h;

  h = hlManager->getHl(mode);
  if (h == m_highlight) {
    updateLines();
  } else {
    if (m_highlight != 0L) m_highlight->release();
    h->use();
    m_highlight = h;
    buffer->setHighlight(m_highlight);
    makeAttribs();
  }

  KateView *view;
  for (view = myViews.first(); view != 0L; view = myViews.next() )
     {
         view->setFoldingMarkersOn(m_highlight->allowsFolding());
     }

  emit(hlChanged());

  return true;
}

uint KateDocument::hlModeCount ()
{
  return HlManager::self()->highlights();
}

QString KateDocument::hlModeName (uint mode)
{
  return HlManager::self()->hlName (mode);
}

QString KateDocument::hlModeSectionName (uint mode)
{
  return HlManager::self()->hlSection (mode);
}

void KateDocument::setDontChangeHlOnSave()
{
  hlSetByUser = true;
}

//
// KTextEditor::ConfigInterface stuff
//

void KateDocument::readConfig(KConfig *config)
{
  _configFlags = config->readNumEntry("ConfigFlags", _configFlags);

  myWordWrap = config->readBoolEntry("Word Wrap On", false);
  myWordWrapAt = config->readNumEntry("Word Wrap At", 80);

  setTabWidth(config->readNumEntry("TabWidth", 8));
  setUndoSteps(config->readNumEntry("UndoSteps", 256));
  setFont (ViewFont,config->readFontEntry("Font", &viewFont.myFont));
  setFont (PrintFont,config->readFontEntry("PrintFont", &printFont.myFont));

  colors[0] = config->readColorEntry("Color Background", &colors[0]);
  colors[1] = config->readColorEntry("Color Selected", &colors[1]);
  colors[2] = config->readColorEntry("Color Current Line", &colors[2]);

  if (myWordWrap)
  {
    editStart (false);
    wrapText (myWordWrapAt);
    editEnd ();
    setModified(false);
    emit textChanged ();
  }

  tagAll();
  updateEditAccels();
}

void KateDocument::writeConfig(KConfig *config)
{
  config->writeEntry("ConfigFlags",_configFlags);

  config->writeEntry("Word Wrap On", myWordWrap);
  config->writeEntry("Word Wrap At", myWordWrapAt);
  config->writeEntry("UndoSteps", myUndoSteps);
  config->writeEntry("TabWidth", tabChars);
  config->writeEntry("Font", viewFont.myFont);
  config->writeEntry("PrintFont", printFont.myFont);
  config->writeEntry("Color Background", colors[0]);
  config->writeEntry("Color Selected", colors[1]);
  config->writeEntry("Color Current Line", colors[2]);
}

void KateDocument::readConfig()
{
  KConfig *config = KateFactory::instance()->config();
  config->setGroup("Kate Document");
  readConfig (config);
  config->sync();
}

void KateDocument::writeConfig()
{
  KConfig *config = KateFactory::instance()->config();
  config->setGroup("Kate Document");
  writeConfig (config);
  config->sync();
}

void KateDocument::readSessionConfig(KConfig *config)
{
  // enable the setMark function to set marks for lines > lastLine !!!
  restoreMarks = true;

  m_url = config->readEntry("URL"); // ### doesn't this break the encoding? (Simon)
  internalSetHlMode(hlManager->nameFind(config->readEntry("Highlight")));

  // restore bookmarks
  QValueList<int> l = config->readIntListEntry("Bookmarks");
  if ( l.count() )
  {
    for (uint i=0; i < l.count(); i++)
      setMark( l[i], KateDocument::markType01 );
  }

  restoreMarks = false;
}

void KateDocument::writeSessionConfig(KConfig *config)
{
  config->writeEntry("URL", m_url.url() ); // ### encoding?? (Simon)
  config->writeEntry("Highlight", m_highlight->name());
  // anders: save bookmarks
  QValueList<int> ml;
  for (uint i=0; i < myMarks.count(); i++) {
    if ( myMarks.at(i)->type == 1) // only save bookmarks
     ml << myMarks.at(i)->line;
  }
  if (ml.count() )
    config->writeEntry("Bookmarks", ml);
}

void KateDocument::configDialog()
{
  KWin kwin;

  KDialogBase *kd = new KDialogBase(KDialogBase::IconList,
                                    i18n("Configure"),
                                    KDialogBase::Ok | KDialogBase::Cancel |
                                    KDialogBase::Help ,
                                    KDialogBase::Ok, kapp->mainWidget());

  // color options
  QVBox *page=kd->addVBoxPage(i18n("Colors"), i18n("Colors"),
                              BarIcon("colorize", KIcon::SizeMedium) );
  Kate::ConfigPage *mcolorConfigPage = colorConfigPage(page);

  page = kd->addVBoxPage(i18n("Fonts"), i18n("Fonts Settings"),
                              BarIcon("fonts", KIcon::SizeMedium) );
  Kate::ConfigPage *mfontConfigPage = fontConfigPage(page);

  // indent options
  page=kd->addVBoxPage(i18n("Indent"), i18n("Indent Options"),
                       BarIcon("rightjust", KIcon::SizeMedium) );
  Kate::ConfigPage *mindentConfigPage = indentConfigPage(page);

  // select options
  page=kd->addVBoxPage(i18n("Select"), i18n("Selection Behavior"),
                       BarIcon("misc") );
  Kate::ConfigPage *mselectConfigPage = selectConfigPage(page);

  // edit options
  page=kd->addVBoxPage(i18n("Edit"), i18n("Editing Options"),
                       BarIcon("edit", KIcon::SizeMedium ) );
  Kate::ConfigPage *meditConfigPage = editConfigPage (page);

  // Cursor key options
  page=kd->addVBoxPage(i18n("Keyboard"), i18n("Keyboard Configuration"),
                       BarIcon("edit", KIcon::SizeMedium ) );
  Kate::ConfigPage *mkeysConfigPage = keysConfigPage (page);

  kwin.setIcons(kd->winId(), kapp->icon(), kapp->miniIcon());

  page=kd->addVBoxPage(i18n("Highlighting"),i18n("Highlighting Configuration"),
                        BarIcon("edit",KIcon::SizeMedium));
  Kate::ConfigPage *mhlConfigPage = hlConfigPage (page);

  if (kd->exec())
  {
    mcolorConfigPage->apply();
    mfontConfigPage->apply();
    mindentConfigPage->apply();
    mselectConfigPage->apply();
    meditConfigPage->apply();
    mkeysConfigPage->apply();
    mhlConfigPage->apply();

    // save the config, reload it to update doc + all views
    writeConfig();
    readConfig();
  }

  delete kd;
}

uint KateDocument::mark (uint line)
{
  if (myMarks.isEmpty())
    return 0;

  if (line > lastLine())
    return 0;

  for (uint z=0; z<myMarks.count(); z++)
    if (myMarks.at(z)->line == line)
      return myMarks.at(z)->type;

  return 0;
}

void KateDocument::setMark (uint line, uint markType)
{
  if ((!restoreMarks) && (line > lastLine()))
    return;

  bool b = false;

  for (uint z=0; z<myMarks.count(); z++)
    if (myMarks.at(z)->line == line)
    {
      myMarks.at(z)->type=markType;
      b = true;
    }

  if (!b)
  {
    KTextEditor::Mark *mark = new KTextEditor::Mark;
    mark->line = line;
    mark->type = markType;
    myMarks.append (mark);
  }

  emit marksChanged ();

  tagLines (line,line);
}

void KateDocument::clearMark (uint line)
{
  if (myMarks.isEmpty())
    return;

  if (line > lastLine())
    return;

  for (uint z=0; z<myMarks.count(); z++)
    if (myMarks.at(z)->line == line)
    {
      myMarks.remove(z);
      emit marksChanged ();

      tagLines (line,line);
    }
}

void KateDocument::addMark (uint line, uint markType)
{
  if (line > lastLine())
    return;

  bool b = false;

  for (uint z=0; z<myMarks.count(); z++)
    if (myMarks.at(z)->line == line)
    {
      myMarks.at(z)->type=myMarks.at(z)->type | markType;
      b = true;
    }

  if (!b)
  {
    KTextEditor::Mark *mark = new KTextEditor::Mark;
    mark->line = line;
    mark->type = markType;
    myMarks.append (mark);
  }

  emit marksChanged ();

  tagLines (line,line);
}

void KateDocument::removeMark (uint line, uint markType)
{
  if (myMarks.isEmpty())
    return;

  if (line > lastLine())
    return;

  for (uint z=0; z<myMarks.count(); z++)
    if (myMarks.at(z)->line == line)
    {
      myMarks.at(z)->type=myMarks.at(z)->type & ~markType;
      if (myMarks.at(z)->type == 0)
        myMarks.remove(z);

      emit marksChanged ();
    }

  tagLines (line,line);
}

QPtrList<KTextEditor::Mark> KateDocument::marks ()
{
  return myMarks;
}

void KateDocument::clearMarks ()
{
  if (myMarks.isEmpty())
    return;

  while (myMarks.count() > 0)
  {
    tagLines (myMarks.at (0)->line, myMarks.at (0)->line);
    myMarks.remove((uint)0);
  }

  emit marksChanged ();
}

//
// KTextEditor::PrintInterface stuff
//

bool KateDocument::printDialog ()
{
  KPrinter printer;
  if (!docName().isEmpty())
    printer.setDocName(docName());
  else if (!url().isEmpty())
    printer.setDocName(url().fileName());
  else
    printer.setDocName(i18n("Untitled"));

   if ( printer.setup( kapp->mainWidget() ) )
   {
     QPainter paint( &printer );
     QPaintDeviceMetrics pdm( &printer );

     uint y = 0;
     uint lineCount = 0;
     uint maxWidth = pdm.width();
     int startCol = 0;
     int endCol = 0;
     bool needWrap = true;

     while (  lineCount <= lastLine()  )
     {
       startCol = 0;
       endCol = 0;
       needWrap = true;

       while (needWrap)
       {
         if (y+printFont.fontHeight >= (uint)pdm.height() )
         {
           printer.newPage();
           y=0;
         }

         endCol = textWidth (buffer->line(lineCount), startCol, maxWidth, 0, PrintFont, &needWrap);
         paintTextLine ( paint, lineCount, startCol, endCol, 0, y, 0, maxWidth, -1, 0, false, false, false, PrintFont, false, true );
         startCol = endCol;
         y += printFont.fontHeight;
       }

       lineCount++;
     }

     return true;
  }

  return false;
}

bool KateDocument::print ()
{
  return printDialog ();
}

//
// KParts::ReadWrite stuff
//

bool KateDocument::openFile()
{
  fileInfo->setFile (m_file);
  setMTime();

  if (!fileInfo->exists() || !fileInfo->isReadable())
    return false;

  clear();
  buffer->insertFile(0, m_file, KGlobal::charsets()->codecForName(myEncoding));

  setMTime();

  if (myWordWrap)
  {
    editStart (false);
    wrapText (myWordWrapAt);
    editEnd ();
    setModified(false);
    emit textChanged ();
  }

  int hl = hlManager->wildcardFind( m_file );

  if (hl == -1)
  {
    // fill the detection buffer with the contents of the text
    // anders: I fixed this to work :^)
    const int HOWMANY = 1024;
    QByteArray buf(HOWMANY);
    int bufpos = 0, len;
    for (uint i=0; i < buffer->count(); i++)
    {
      QString line = buffer->plainLine(i);
      len = line.length() + 1; // space for a newline - seemingly not required by kmimemagic, but nicer for debugging.
//kdDebug(13020)<<"openFile(): collecting a buffer for hlManager->mimeFind(): found "<<len<<" bytes in line "<<i<<endl;
      if (bufpos + len > HOWMANY) len = HOWMANY - bufpos;
//kdDebug(13020)<<"copying "<<len<<"bytes."<<endl;
      memcpy(&buf[bufpos], (line+"\n").latin1(), len);
      bufpos += len;
      if (bufpos >= HOWMANY) break;
    }
//kdDebug(13020)<<"openFile(): calling hlManager->mimeFind() with data:"<<endl<<buf.data()<<endl<<"--"<<endl;
    hl = hlManager->mimeFind( buf, m_file );
  }

  internalSetHlMode(hl);

  updateLines();
  updateViews();

  emit fileNameChanged();

  return true;
}

bool KateDocument::saveFile()
{
  QFile f( m_file );
  if ( !f.open( IO_WriteOnly ) )
    return false; // Error

  QTextStream stream(&f);

  stream.setEncoding(QTextStream::RawUnicode); // disable Unicode headers
  stream.setCodec(KGlobal::charsets()->codecForName(myEncoding)); // this line sets the mapper to the correct codec

  int maxLine = numLines();
  int line = 0;
  while(true)
  {
    stream << textLine(line);
    line++;
    if (line >= maxLine) break;

    if (eolMode == KateDocument::eolUnix) stream << "\n";
    else if (eolMode == KateDocument::eolDos) stream << "\r\n";
    else if (eolMode == KateDocument::eolMacintosh) stream << '\r';
  };
  f.close();

  fileInfo->setFile (m_file);
  setMTime();

  if (!hlSetByUser)
  {
  int hl = hlManager->wildcardFind( m_file );

  if (hl == -1)
  {
    // fill the detection buffer with the contents of the text
    const int HOWMANY = 1024;
    QByteArray buf(HOWMANY);
    int bufpos = 0, len;
    for (uint i=0; i < buffer->count(); i++)
    {
      TextLine::Ptr textLine = buffer->line(i);
      len = textLine->length();
      if (bufpos + len > HOWMANY) len = HOWMANY - bufpos;
      memcpy(&buf[bufpos], textLine->text(), len);
      bufpos += len;
      if (bufpos >= HOWMANY) break;
    }

    hl = hlManager->mimeFind( buf, m_file );
  }

  internalSetHlMode(hl);
  }
  emit fileNameChanged ();

  return (f.status() == IO_Ok);
}

void KateDocument::setReadWrite( bool rw )
{
  KTextEditor::View *view;

  if (rw == readOnly)
  {
    readOnly = !rw;
    KParts::ReadWritePart::setReadWrite (rw);
    for (view = myViews.first(); view != 0L; view = myViews.next() ) {
      emit static_cast<KateView *>( view )->newStatus();
    }
  }
}

bool KateDocument::isReadWrite() const
{
  return !readOnly;
}

void KateDocument::setModified(bool m) {
  KTextEditor::View *view;

  if (m != modified) {
    modified = m;
    KParts::ReadWritePart::setModified (m);
    for (view = myViews.first(); view != 0L; view = myViews.next() ) {
      emit static_cast<KateView *>( view )->newStatus();
    }
    emit modifiedChanged ();
  }
}

bool KateDocument::isModified() const {
  return modified;
}

//
// Kate specific stuff ;)
//

void KateDocument::setFont (WhichFont wf,QFont font)
{
  FontStruct *fs=(wf==ViewFont)?&viewFont:&printFont;

  fs->myFont = font;
  fs->myFontBold = QFont (font);
  fs->myFontBold.setBold (true);

  fs->myFontItalic = QFont (font);
  fs->myFontItalic.setItalic (true);

  fs->myFontBI = QFont (font);
  fs->myFontBI.setBold (true);
  fs->myFontBI.setItalic (true);

  fs->myFontMetrics = KateFontMetrics (fs->myFont);
  fs->myFontMetricsBold = KateFontMetrics (fs->myFontBold);
  fs->myFontMetricsItalic = KateFontMetrics (fs->myFontItalic);
  fs->myFontMetricsBI = KateFontMetrics (fs->myFontBI);

  fs->updateFontData(tabChars);
  if (wf==ViewFont)
  {
    updateFontData();
    updateViews();
  }
}

void KateDocument::slotBufferUpdateHighlight(uint from, uint to)
{
  if (to > m_highlightedEnd)
     m_highlightedEnd = to;
  uint till = from + 100;
  if (till > m_highlightedEnd)
     till = m_highlightedEnd;
  buffer->updateHighlighting(from, till, false);
  m_highlightedTill = till;
  if (m_highlightedTill >= m_highlightedEnd)
  {
      m_highlightedTill = 0;
      m_highlightedEnd = 0;
      m_highlightTimer->stop();
  }
  else
  {
      m_highlightTimer->start(100, true);
  }
}

void KateDocument::slotBufferUpdateHighlight()
{
  uint till = m_highlightedTill + 1000;

  uint max = numLines();
  if (m_highlightedEnd > max)
    m_highlightedEnd = max;

  if (till > m_highlightedEnd)
     till = m_highlightedEnd;
  buffer->updateHighlighting(m_highlightedTill, till, false);
  m_highlightedTill = till;
  if (m_highlightedTill >= m_highlightedEnd)
  {
      m_highlightedTill = 0;
      m_highlightedEnd = 0;
      m_highlightTimer->stop();
  }
  else
  {
      m_highlightTimer->start(100, true);
  }
}

int KateDocument::textLength(int line) const {
  return buffer->plainLine(line).length();
}

void KateDocument::setTabWidth(int chars) {
  if (tabChars == chars) return;
  if (chars < 1) chars = 1;
  if (chars > 16) chars = 16;
  tabChars = chars;
  printFont.updateFontData(tabChars);
  viewFont.updateFontData(tabChars);
  updateFontData();
}

void KateDocument::setNewDoc( bool m )
{
  if ( m != newDoc )
  {
    newDoc = m;
  }
}

bool KateDocument::isNewDoc() const {
  return newDoc;
}

void KateDocument::makeAttribs()
{
  hlManager->makeAttribs(this, m_highlight);
  updateFontData();
  updateLines();
}

void KateDocument::updateFontData() {
  KateView *view;

  for (view = myViews.first(); view != 0L; view = myViews.next() ) {
     view->myViewInternal->tagAll();
  }
}

void KateDocument::internalHlChanged() { //slot
  makeAttribs();
}

void KateDocument::addView(KTextEditor::View *view) {
  myViews.append( (KateView *) view  );
  _views.append( view );
  myActiveView = (KateView *) view;
}

void KateDocument::removeView(KTextEditor::View *view) {
  if (myActiveView == view)
    myActiveView = 0L;

  myViews.removeRef( (KateView *) view );
  _views.removeRef( view  );
}

void KateDocument::addCursor(KTextEditor::Cursor *cursor) {
  myCursors.append( cursor );
}

void KateDocument::removeCursor(KTextEditor::Cursor *cursor) {
  myCursors.removeRef( cursor  );
}

bool KateDocument::ownedView(KateView *view) {
  // do we own the given view?
  return (myViews.containsRef(view) > 0);
}

bool KateDocument::isLastView(int numViews) {
  return ((int) myViews.count() == numViews);
}

uint KateDocument::textWidth(const TextLine::Ptr &textLine, int cursorX,WhichFont wf)
{
  if (!textLine)
    return 0;

  if (cursorX < 0)
    cursorX = textLine->length();

  int x;
  int z;
  QChar ch;
  Attribute *a;
  FontStruct *fs=(wf==ViewFont)?&viewFont:&printFont;

  x = 0;
  for (z = 0; z < cursorX; z++) {
    ch = textLine->getChar(z);
    a = attribute(textLine->attribute(z));

    if (ch == '\t')
      x += fs->m_tabWidth;
    else if (a->bold && a->italic)
      x += fs->myFontMetricsBI.width(ch);
    else if (a->bold)
      x += fs->myFontMetricsBold.width(ch);
    else if (a->italic)
      x += fs->myFontMetricsItalic.width(ch);
    else
      x += fs->myFontMetrics.width(ch);
  }
  return x;
}

uint KateDocument::textWidth(const TextLine::Ptr &textLine, uint startcol, uint maxwidth, uint wrapsymwidth, WhichFont wf, bool *needWrap)
{
  QChar ch;
  Attribute *a;
  FontStruct *fs=(wf==ViewFont)?&viewFont:&printFont;
  uint x = 0;
  uint endcol = 0;
  uint endcolwithsym = 0;

  *needWrap = false;

  for (uint z = startcol; z < textLine->length(); z++)
  {
    ch = textLine->getChar(z);
    a = attribute(textLine->attribute(z));

    if (ch == '\t')
      x += fs->m_tabWidth;
    else if (a->bold && a->italic)
      x += fs->myFontMetricsBI.width(ch);
    else if (a->bold)
      x += fs->myFontMetricsBold.width(ch);
    else if (a->italic)
      x += fs->myFontMetricsItalic.width(ch);
    else
      x += fs->myFontMetrics.width(ch);

    if (x <= maxwidth-wrapsymwidth )
      endcolwithsym = z+1;

    if (x <= maxwidth)
      endcol = z+1;

    if (x >= maxwidth)
    {
      *needWrap = true;
      break;
    }
  }

  if (*needWrap)
    return endcolwithsym;
  else
    return endcol;
}

uint KateDocument::textWidth(KateTextCursor &cursor)
{
  if (cursor.col < 0)
     cursor.col = 0;
  if (cursor.line < 0)
     cursor.line = 0;
  if (cursor.line >= (int)numLines())
     cursor.line = lastLine();
  return textWidth(buffer->line(cursor.line),cursor.col);
}

uint KateDocument::textWidth( KateTextCursor &cursor, int xPos,WhichFont wf)
{
  bool wrapCursor = configFlags() & KateDocument::cfWrapCursor;
  int len;
  int x, oldX;
  int z;
  QChar ch;
  Attribute *a;

  FontStruct *fs=(wf==ViewFont)?&viewFont:&printFont;

  if (cursor.line < 0) cursor.line = 0;
  if (cursor.line > (int)lastLine()) cursor.line = lastLine();
  TextLine::Ptr textLine = buffer->line(cursor.line);
  len = textLine->length();

  x = oldX = z = 0;
  while (x < xPos && (!wrapCursor || z < len)) {
    oldX = x;
    ch = textLine->getChar(z);
    a = attribute(textLine->attribute(z));

    if (ch == '\t')
      x += fs->m_tabWidth;
    else if (a->bold && a->italic)
      x += fs->myFontMetricsBI.width(ch);
    else if (a->bold)
      x += fs->myFontMetricsBold.width(ch);
    else if (a->italic)
      x += fs->myFontMetricsItalic.width(ch);
    else
      x += fs->myFontMetrics.width(ch);

    z++;
  }
  if (xPos - oldX < x - xPos && z > 0) {
    z--;
    x = oldX;
  }
  cursor.col = z;
  return x;
}

uint KateDocument::textPos(const TextLine::Ptr &textLine, int xPos,WhichFont wf) {
  int x, oldX;
  int z;
  QChar ch;
  Attribute *a;

  FontStruct *fs=(wf==ViewFont)?&viewFont:&printFont;

  x = oldX = z = 0;
  while (x < xPos) { // && z < len) {
    oldX = x;
    ch = textLine->getChar(z);
    a = attribute(textLine->attribute(z));

    if (ch == '\t')
      x += fs->m_tabWidth;
    else if (a->bold && a->italic)
      x += fs->myFontMetricsBI.width(ch);
    else if (a->bold)
      x += fs->myFontMetricsBold.width(ch);
    else if (a->italic)
      x += fs->myFontMetricsItalic.width(ch);
    else
      x += fs->myFontMetrics.width(ch);

    z++;
  }
  if (xPos - oldX < x - xPos && z > 0) {
    z--;
   // newXPos = oldX;
  }// else newXPos = x;
  return z;
}

uint KateDocument::textHeight(WhichFont wf) {
  return numLines()*((wf==ViewFont)?viewFont.fontHeight:printFont.fontHeight);
}

uint KateDocument::currentColumn( const KateTextCursor& cursor )
{
  TextLine::Ptr t = buffer->line(cursor.line);

  if (t)
    return t->cursorX(cursor.col,tabChars);
  else
    return 0;
}

bool KateDocument::insertChars ( int line, int col, const QString &chars, KateView *view )
{
  int z, pos, l;
  bool onlySpaces;
  QChar ch;
  QString buf;

  int savedCol=col;
  int savedLine=line;
  QString savedChars(chars);

  TextLine::Ptr textLine = buffer->line(line);

  pos = 0;
  onlySpaces = true;
  for (z = 0; z < (int) chars.length(); z++) {
    ch = chars[z];
    if (ch == '\t' && _configFlags & KateDocument::cfReplaceTabs) {
      l = tabChars - (textLine->cursorX(col, tabChars) % tabChars);
      while (l > 0) {
        buf.insert(pos, ' ');
        pos++;
        l--;
      }
    } else if (ch.isPrint() || ch == '\t') {
      buf.insert(pos, ch);
      pos++;
      if (ch != ' ') onlySpaces = false;
      if (_configFlags & KateDocument::cfAutoBrackets) {
        if (ch == '(') buf.insert(pos, ')');
        if (ch == '[') buf.insert(pos, ']');
        if (ch == '{') buf.insert(pos, '}');
      }
    }
  }
  //pos = cursor increment

  //return false if nothing has to be inserted
  if (buf.isEmpty()) return false;

  editStart ();

  if (_configFlags & KateDocument::cfDelOnInput)
  {
    if (hasSelection())
    {
      removeSelectedText();
      line = view->myViewInternal->cursorCache.line;
      col = view->myViewInternal->cursorCache.col;
    }
  }

  if (_configFlags & KateDocument::cfOvr)
  {
    if ((col+buf.length()) <= textLine->length())
      removeText (line, col, line, col+buf.length());
    else
      removeText (line, col, line, textLine->length());
  }

  insertText (line, col, buf);
  col += pos;

  // editEnd will set the cursor from this cache right ;))
  view->myViewInternal->cursorCache.line = line;
  view->myViewInternal->cursorCache.col = col;
  view->myViewInternal->cursorCacheChanged = true;

  editEnd ();

  emit charactersInteractivelyInserted(savedLine,savedCol,savedChars);
  return true;
}

QString tabString(int pos, int tabChars)
{
  QString s;
  while (pos >= tabChars) {
    s += '\t';
    pos -= tabChars;
  }
  while (pos > 0) {
    s += ' ';
    pos--;
  }
  return s;
}

void KateDocument::newLine( KateTextCursor& c )
{

  if (!(_configFlags & KateDocument::cfAutoIndent)) {
    insertText( c.line, c.col, "\n" );
    c.line++;
    c.col = 0;
  } else {
    TextLine::Ptr textLine = buffer->line(c.line);
    int pos = textLine->firstChar();
    if (c.col < pos) c.col = pos; // place cursor on first char if before


    int y = c.line;
    while ((y > 0) && (pos < 0)) { // search a not empty text line
      textLine = buffer->line(--y);
      pos = textLine->firstChar();
    }
    insertText (c.line, c.col, "\n");
    c.line++;
    c.col = 0;
    if (pos > 0) {
      pos = textLine->cursorX(pos, tabChars);
      QString s = tabString(pos, (_configFlags & KateDocument::cfSpaceIndent) ? 0xffffff : tabChars);
      insertText (c.line, c.col, s);
      pos = s.length();
      c.col = pos;
    }
  }
}

void KateDocument::killLine( uint line )
{
  removeLine( line );
//  regionTree->lineHasBeenRemoved(c.cursor.line);// is this the right place ?
}


void KateDocument::transpose( const KateTextCursor& cursor)
{
  TextLine::Ptr textLine = buffer->line(cursor.line);
  uint line = cursor.line;
  uint col = cursor.col;

  if (!textLine)
    return;

  QString s;
  if (col != 0)  //there is a special case for this one
    --col;

  //clever swap code if first character on the line swap right&left
  //otherwise left & right
  s.append (textLine->getChar(col+1));
  s.append (textLine->getChar(col));
  //do the swap
  
  // do it right, never ever manipulate a textline
  editStart ();
  editRemoveText (line, col, 2);
  editInsertText (line, col, s);

  editEnd ();
}

void KateDocument::backspace( const KateTextCursor& c )
{
  uint col = QMAX( c.col, 0 );
  uint line = QMAX( c.line, 0 );

  if ((col == 0) && (line == 0))
    return;

  if (col > 0)
  {
    if (!(_configFlags & KateDocument::cfBackspaceIndents))
    {
      // ordinary backspace
      //c.cursor.col--;
      removeText(line, col-1, line, col);
    }
    else
    {
      // backspace indents: erase to next indent position
      int l = 1; // del one char

      TextLine::Ptr textLine = buffer->line(line);
      int pos = textLine->firstChar();
      if (pos < 0 || pos >= (int)col)
      {
        // only spaces on left side of cursor
        // search a line with less spaces
        uint y = line;
        while (y > 0)
        {
          textLine = buffer->line(--y);
          pos = textLine->firstChar();

          if (pos >= 0 && pos < (int)col)
          {
            l = col - pos; // del more chars
            break;
          }
        }
      }
      // break effectively jumps here
      //c.cursor.col -= l;
      removeText(line, col-l, line, col);
    }
  }
  else
  {
    // col == 0: wrap to previous line
    if (line >= 1)
    {
      regionTree->lineHasBeenRemoved(line);
      removeText (line-1, buffer->line(line-1)->length(), line, 0);
    }
  }
}

void KateDocument::del( const KateTextCursor& c )
{
  if( c.col < (int) buffer->line(c.line)->length())
  {
    removeText(c.line, c.col, c.line, c.col+1);
  }
  else
  {
    regionTree->lineHasBeenRemoved(c.line);
    removeText(c.line, c.col, c.line+1, 0);
  }
}

void KateDocument::cut()
{
  if (!hasSelection())
    return;

  copy();
  removeSelectedText();
}

void KateDocument::copy()
{
  if (!hasSelection())
    return;

  QApplication::clipboard()->setText(selection ());
}

void KateDocument::paste( const KateTextCursor& cursor, KateView* view )
{
  QString s = QApplication::clipboard()->text();

  if (s.isEmpty())
    return;

  editStart ();

  uint line = cursor.line;
  uint col = cursor.col;
  
  insertText( line, col, s );

  // anders: we want to be able to move the cursor to the
  // position at the end of the pasted text,
  // so we calculate that and applies it to c.cursor
  // This may not work, when wordwrap gets fixed :(
  TextLine *ln = buffer->line( line );
  int l = s.length();
  while ( l > 0 ) {
    if ( col < ln->length() ) {
      col++;
    } else {
      line++;
      ln = buffer->line( line );
      col = 0;
    }
    l--;
  }

  // editEnd will set the cursor from this cache right ;))
  // Totally breaking the whole idea of the doc view model here...
  view->myViewInternal->cursorCache.line = line;
  view->myViewInternal->cursorCache.col = col;
  view->myViewInternal->cursorCacheChanged = true;

  editEnd();
}

void KateDocument::selectTo( const KateTextCursor& from, const KateTextCursor& to )
{
  if ( selectAnchor.line == -1 )
  {
    // anders: if we allready have a selection, we want to include all of that
    if ( hasSelection() &&
            ( to.line > selectEnd.line || to.line >= selectEnd.line && to.col >= selectEnd.col ) ) {
      selectAnchor.line = selectStart.line;
      selectAnchor.col = selectStart.col;
    }
    else {
      selectAnchor.line = from.line;
      selectAnchor.col = from.col;
    }
  }

  setSelection( selectAnchor.line, selectAnchor.col, to.line, to.col );
}

void KateDocument::selectWord( const KateTextCursor& cursor ) {
  int start, end, len;

  TextLine::Ptr textLine = buffer->line(cursor.line);
  len = textLine->length();
  start = end = cursor.col;
  while (start > 0 && m_highlight->isInWord(textLine->getChar(start - 1))) start--;
  while (end < len && m_highlight->isInWord(textLine->getChar(end))) end++;
  if (end <= start) return;

  if (!(_configFlags & KateDocument::cfKeepSelection))
    clearSelection ();
  setSelection (cursor.line, start, cursor.line, end);
}

void KateDocument::selectLine( const KateTextCursor& cursor ) {
  if (!(_configFlags & KateDocument::cfKeepSelection))
    clearSelection ();
  setSelection (cursor.line, 0, cursor.line+1, 0);
}

void KateDocument::selectLength( const KateTextCursor& cursor, int length ) {
  int start, end;

  TextLine::Ptr textLine = buffer->line(cursor.line);
  start = cursor.col;
  end = start + length;
  if (end <= start) return;

  if (!(_configFlags & KateDocument::cfKeepSelection))
    clearSelection ();
  setSelection (cursor.line, start, cursor.line, end);
}

void KateDocument::doIndent( uint line, int change)
{
  editStart ();

  if (!hasSelection()) {
    // single line
    optimizeLeadingSpace( line, _configFlags, change);
  }
  else
  {
    int sl = selectStart.line;
    int el = selectEnd.line;
    int ec = selectEnd.col;

    if ((ec == 0) && ((el-1) >= 0))
    {
      el--;
    }

    // entire selection
    TextLine::Ptr textLine;
    int line, z;
    QChar ch;

    if (_configFlags & KateDocument::cfKeepIndentProfile && change < 0) {
      // unindent so that the existing indent profile doesnt get screwed
      // if any line we may unindent is already full left, don't do anything
      for (line = sl; line <= el; line++) {
        textLine = buffer->line(line);
        if (lineSelected(line) || lineHasSelected(line)) {
          for (z = 0; z < tabChars; z++) {
            ch = textLine->getChar(z);
            if (ch == '\t') break;
            if (ch != ' ') {
              change = 0;
              goto jumpOut;
            }
          }
        }
      }
      jumpOut:;
    }

    for (line = sl; line <= el; line++) {
      if (lineSelected(line) || lineHasSelected(line)) {
        optimizeLeadingSpace(line, _configFlags, change);
      }
    }
  }

  editEnd ();
}

/*
  Optimize the leading whitespace for a single line.
  If change is > 0, it adds indentation units (tabChars)
  if change is == 0, it only optimizes
  If change is < 0, it removes indentation units
  This will be used to indent, unindent, and optimal-fill a line.
  If excess space is removed depends on the flag cfKeepExtraSpaces
  which has to be set by the user
*/
void KateDocument::optimizeLeadingSpace(uint line, int flags, int change) {
  int len;
  int chars, space, okLen;
  QChar ch;
  int extra;
  QString s;
  KateTextCursor cursor;

  TextLine::Ptr textLine = buffer->line(line);
  len = textLine->length();
  space = 0; // length of space at the beginning of the textline
  okLen = 0; // length of space which does not have to be replaced
  for (chars = 0; chars < len; chars++) {
    ch = textLine->getChar(chars);
    if (ch == ' ') {
      space++;
      if (flags & KateDocument::cfSpaceIndent && okLen == chars) okLen++;
    } else if (ch == '\t') {
      space += tabChars - space % tabChars;
      if (!(flags & KateDocument::cfSpaceIndent) && okLen == chars) okLen++;
    } else break;
  }

  space += change*tabChars; // modify space width
  // if line contains only spaces it will be cleared
  if (space < 0 || chars == len) space = 0;

  extra = space % tabChars; // extra spaces which dont fit the indentation pattern
  if (flags & KateDocument::cfKeepExtraSpaces) chars -= extra;

  if (flags & KateDocument::cfSpaceIndent) {
    space -= extra;
    ch = ' ';
  } else {
    space /= tabChars;
    ch = '\t';
  }

  // dont replace chars which are already ok
  cursor.col = QMIN(okLen, QMIN(chars, space));
  chars -= cursor.col;
  space -= cursor.col;
  if (chars == 0 && space == 0) return; //nothing to do

  s.fill(ch, space);

//printf("chars %d insert %d cursor.col %d\n", chars, insert, cursor.col);
  cursor.line = line;
  removeText (cursor.line, cursor.col, cursor.line, cursor.col+chars);
  insertText(cursor.line, cursor.col, s);
}

/*
  Remove a given string at the begining
  of the current line.
*/
bool KateDocument::removeStringFromBegining(int line, QString &str)
{
  TextLine* textline = buffer->line(line);

  if(textline->startingWith(str))
  {
    // Get string lenght
    int length = str.length();

    // Remove some chars
    doRemoveText (line, 0, line, length);

    return true;
  }

  return false;
}

/*
  Remove a given string at the end
  of the current line.
*/
bool KateDocument::removeStringFromEnd(int line, QString &str)
{
  TextLine* textline = buffer->line(line);

  if(textline->endingWith(str))
  {
    // Get string lenght
    int length = str.length();

    // Remove some chars
    doRemoveText (line, 0, line, length);

    return true;
  }

  return false;
}

/*
  Add to the current line a comment line mark at
  the begining.
*/
void KateDocument::addStartLineCommentToSingleLine(int line)
{
  QString commentLineMark = m_highlight->getCommentSingleLineStart() + " ";
  insertText (line, 0, commentLineMark);
}

/*
  Remove from the current line a comment line mark at
  the begining if there is one.
*/
bool KateDocument::removeStartLineCommentFromSingleLine(int line)
{
  QString shortCommentMark = m_highlight->getCommentSingleLineStart();
  QString longCommentMark = shortCommentMark + " ";

  editStart();

  // Try to remove the long comment mark first
  bool removed = (removeStringFromBegining(line, longCommentMark)
                  || removeStringFromBegining(line, shortCommentMark));

  editEnd();

  return removed;
}

/*
  Add to the current line a start comment mark at the
 begining and a stop comment mark at the end.
*/
void KateDocument::addStartStopCommentToSingleLine(int line)
{
  QString startCommentMark = m_highlight->getCommentStart() + " ";
  QString stopCommentMark = " " + m_highlight->getCommentEnd();

  editStart();

  // Add the start comment mark
  doInsertText (line, 0, startCommentMark);

  // Go to the end of the line
  TextLine* textline = buffer->line(line);
  int col = textline->length();

  // Add the stop comment mark
  doInsertText (line, col, stopCommentMark);

  editEnd();
}

/*
  Remove from the current line a start comment mark at
  the begining and a stop comment mark at the end.
*/
bool KateDocument::removeStartStopCommentFromSingleLine(int line)
{
  QString shortStartCommentMark = m_highlight->getCommentStart();
  QString longStartCommentMark = shortStartCommentMark + " ";
  QString shortStopCommentMark = m_highlight->getCommentEnd();
  QString longStopCommentMark = " " + shortStopCommentMark;

  editStart();

  // Try to remove the long start comment mark first
  bool removedStart = (removeStringFromBegining(line, longStartCommentMark)
                       || removeStringFromBegining(line, shortStartCommentMark));

  // Try to remove the long stop comment mark first
  bool removedStop = (removeStringFromEnd(line, longStopCommentMark)
                      || removeStringFromEnd(line, shortStopCommentMark));

  editEnd();

  return (removedStart || removedStop);
}

/*
  Add to the current selection a start comment
 mark at the begining and a stop comment mark
 at the end.
*/
void KateDocument::addStartStopCommentToSelection()
{
  QString startComment = m_highlight->getCommentStart();
  QString endComment = m_highlight->getCommentEnd();

  int sl = selectStart.line;
  int el = selectEnd.line;
  int sc = selectStart.col;
  int ec = selectEnd.col;

  if ((ec == 0) && ((el-1) >= 0))
  {
    el--;
    ec = buffer->line (el)->length();
  }

  editStart();

  doInsertText (el, ec, endComment);
  doInsertText (sl, sc, startComment);

  editEnd ();

  // Set the new selection
  ec += endComment.length() + ( (el == sl) ? startComment.length() : 0 );
  setSelection(sl, sc, el, ec);
}

/*
  Add to the current selection a comment line
 mark at the begining of each line.
*/
void KateDocument::addStartLineCommentToSelection()
{
  QString commentLineMark = m_highlight->getCommentSingleLineStart() + " ";

  int sl = selectStart.line;
  int el = selectEnd.line;

  if ((selectEnd.col == 0) && ((el-1) >= 0))
  {
    el--;
  }

  editStart();

  // For each line of the selection
  for (int z = el; z >= sl; z--) {
    doInsertText (z, 0, commentLineMark);
  }

  editEnd ();

  // Set the new selection
  selectEnd.col += ( (el == selectEnd.line) ? commentLineMark.length() : 0 );
  setSelection(selectStart.line, 0,
	       selectEnd.line, selectEnd.col);
}

/*
  Find the position (line and col) of the next char
  that is not a space. Return true if found.
*/
bool KateDocument::nextNonSpaceCharPos(int &line, int &col)
{
  for(; line < (int)buffer->count(); line++) {
    TextLine::Ptr textLine = buffer->line(line);
    col = textLine->nextNonSpaceChar(col);
    if(col != -1)
      return true; // Next non-space char found 
    col = 0;
  }
  // No non-space char found
  line = -1;
  col = -1;
  return false;
}

/*
  Find the position (line and col) of the previous char
  that is not a space. Return true if found.
*/
bool KateDocument::previousNonSpaceCharPos(int &line, int &col)
{
  for(; line >= 0; line--) {
    TextLine::Ptr textLine = buffer->line(line);
    col = textLine->previousNonSpaceChar(col);
    if(col != -1)
      return true; // Previous non-space char found 
    col = 0;
  }
  // No non-space char found
  line = -1;
  col = -1;
  return false;
}

/*
  Remove from the selection a start comment mark at
  the begining and a stop comment mark at the end.
*/
bool KateDocument::removeStartStopCommentFromSelection()
{
  QString startComment = m_highlight->getCommentStart();
  QString endComment = m_highlight->getCommentEnd();

  int sl = selectStart.line;
  int el = selectEnd.line;
  int sc = selectStart.col;
  int ec = selectEnd.col;

  // The selection ends on the char before selectEnd 
  if (ec != 0) {
    ec--;
  } else {
    if (el > 0) {
      el--;
      ec = buffer->line(el)->length() - 1;
    }
  }

  int startCommentLen = startComment.length();
  int endCommentLen = endComment.length();

  // had this been perl or sed: s/^\s*$startComment(.+?)$endComment\s*/$1/

  bool remove = nextNonSpaceCharPos(sl, sc)
      && buffer->line(sl)->stringAtPos(sc, startComment)
      && previousNonSpaceCharPos(el, ec)
      && ( (ec - endCommentLen + 1) >= 0 )
      && buffer->line(el)->stringAtPos(ec - endCommentLen + 1, endComment);
 
  if (remove) {
    editStart();

    doRemoveText (el, ec - endCommentLen + 1, el, ec + 1);
    doRemoveText (sl, sc, sl, sc + startCommentLen);

    editEnd ();

    // Set the new selection
    ec -= endCommentLen + ( (el == sl) ? startCommentLen : 0 );
    setSelection(sl, sc, el, ec + 1);
  }

  return remove;
}

/*
  Remove from the begining of each line of the
  selection a start comment line mark.
*/
bool KateDocument::removeStartLineCommentFromSelection()
{
  QString shortCommentMark = m_highlight->getCommentSingleLineStart();
  QString longCommentMark = shortCommentMark + " ";

  int sl = selectStart.line;
  int el = selectEnd.line;

  if ((selectEnd.col == 0) && ((el-1) >= 0))
  {
    el--;
  }

  // Find out how many char will be removed from the last line
  int removeLength = 0;
  if (buffer->line(el)->startingWith(longCommentMark))
    removeLength = longCommentMark.length();
  else if (buffer->line(el)->startingWith(shortCommentMark))
    removeLength = shortCommentMark.length();    

  bool removed = false;

  editStart();

  // For each line of the selection
  for (int z = el; z >= sl; z--)
  {
    // Try to remove the long comment mark first
    removed = (removeStringFromBegining(z, longCommentMark)
                 || removeStringFromBegining(z, shortCommentMark)
                 || removed);
  }

  editEnd();

  if(removed) {
    // Set the new selection
    selectEnd.col -= ( (el == selectEnd.line) ? removeLength : 0 );
    setSelection(selectStart.line, selectStart.col,
	       selectEnd.line, selectEnd.col);
  }

  return removed;
}

/*
  Comment or uncomment the selection or the current
  line if there is no selection.
*/
void KateDocument::doComment( uint line, int change)
{
  bool hasStartLineCommentMark = !(m_highlight->getCommentSingleLineStart().isEmpty());
  bool hasStartStopCommentMark = ( !(m_highlight->getCommentStart().isEmpty())
                                   && !(m_highlight->getCommentEnd().isEmpty()) );

  bool removed = false;

  if (change > 0)
  {
    if ( !hasSelection() )
    {
      if ( hasStartLineCommentMark )
        addStartLineCommentToSingleLine(line);
      else if ( hasStartStopCommentMark )
        addStartStopCommentToSingleLine(line);
    }
    else
    {
      // anders: prefer single line comment to avoid nesting probs
      // If the selection starts after first char in the first line
      // or ends before the last char of the last line, we may use
      // multiline comment markers.
      // TODO We should try to detect nesting.
      //    - if selection ends at col 0, most likely she wanted that 
      // line ignored
      if ( hasStartStopCommentMark &&
           ( !hasStartLineCommentMark || (
             ( selectStart.col > buffer->line( selectStart.line )->firstChar() ) ||
               ( selectEnd.col < ((int)buffer->line( selectEnd.line )->length()) )
         ) ) )
        addStartStopCommentToSelection();
      else if ( hasStartLineCommentMark )
        addStartLineCommentToSelection();
    }
  }
  else
  {
    if ( !hasSelection() )
    {
      removed = ( hasStartLineCommentMark
                  && removeStartLineCommentFromSingleLine(line) )
        || ( hasStartStopCommentMark
             && removeStartStopCommentFromSingleLine(line) );
    }
    else
    {
      // anders: this seems like it will work with above changes :)
      removed = ( hasStartLineCommentMark
                  && removeStartLineCommentFromSelection() )
        || ( hasStartStopCommentMark
             && removeStartStopCommentFromSelection() );
    }
  }
}

QString KateDocument::getWord( const KateTextCursor& cursor ) {
  int start, end, len;

  TextLine::Ptr textLine = buffer->line(cursor.line);
  len = textLine->length();
  start = end = cursor.col;
  while (start > 0 && m_highlight->isInWord(textLine->getChar(start - 1))) start--;
  while (end < len && m_highlight->isInWord(textLine->getChar(end))) end++;
  len = end - start;
  return QString(&textLine->text()[start], len);
}

void KateDocument::tagLines(int start, int end)
{
  for (uint z = 0; z < myViews.count(); z++)
    myViews.at(z)->myViewInternal->tagRealLines(start, end);
}

void KateDocument::tagAll()
{
  for (uint z = 0; z < myViews.count(); z++)
    myViews.at(z)->myViewInternal->tagAll();
}

void KateDocument::updateLines()
{
  buffer->invalidateHighlighting();
}

void KateDocument::updateLines(int startLine, int endLine)
{
  buffer->updateHighlighting(startLine, endLine+1, true);
}

void KateDocument::slotBufferChanged()
{
  updateViews();
}

void KateDocument::updateViews()
{
  if (noViewUpdates)
    return;

  KateView *view;
  
  for (view = myViews.first(); view != 0L; view = myViews.next() )
  {
    view->myViewInternal->updateView();
  }
}

void KateDocument::updateEditAccels()
{
  KateView *view;

  for (view = myViews.first(); view != 0L; view = myViews.next() )
  {
    view->setupEditKeys();
  }

}

QColor &KateDocument::backCol(int x, int y) {
  return (lineColSelected(x,y)) ? colors[1] : colors[0];
 }

QColor &KateDocument::cursorCol(int x, int y)
{
  TextLine::Ptr textLine = buffer->line(y);
  Attribute *a = attribute(textLine->attribute(x));

  if (lineColSelected (y, x))
    return a->selCol;
  else
    return a->col;
}

bool KateDocument::lineColSelected (int line, int col)
{
  if (!blockSelect)
  {
    if ((line > selectStart.line) && (line < selectEnd.line))
      return true;

    if ((line == selectStart.line) && (col >= selectStart.col) && (line < selectEnd.line))
      return true;

    if ((line == selectEnd.line) && (col < selectEnd.col) && (line > selectStart.line))
      return true;

    if ((line == selectEnd.line) && (col < selectEnd.col) && (line == selectStart.line) && (col >= selectStart.col))
      return true;

    if ((line == selectStart.line) && (selectStart.col == 0) && (col < 0))
      return true;
  }
  else
  {
    if ((line >= selectStart.line) && (line <= selectEnd.line) && (col >= selectStart.col) && (col < selectEnd.col))
      return true;
  }

  return false;
}

bool KateDocument::lineSelected (int line)
{
  if (!blockSelect)
  {
    if ((line > selectStart.line) && (line < selectEnd.line))
      return true;

    if ((line == selectStart.line) && (line < selectEnd.line) && (selectStart.col == 0))
      return true;
  }

  return false;
}

bool KateDocument::lineEndSelected (int line)
{
  if (!blockSelect)
  {
    if ((line >= selectStart.line) && (line < selectEnd.line))
      return true;
  }

  return false;
}

bool KateDocument::lineHasSelected (int line)
{
  if (!blockSelect)
  {
    if ((line > selectStart.line) && (line < selectEnd.line))
      return true;

    if ((line == selectStart.line) && (line < selectEnd.line))
      return true;

    if ((line == selectEnd.line) && (line > selectStart.line))
      return true;

    if ((line == selectEnd.line) && (line == selectStart.line) && (selectEnd.col > selectStart.col))
      return true;
  }
  else
  {
    if ((line <= selectEnd.line) && (line >= selectStart.line) && (selectEnd.col > selectStart.col))
      return true;
  }

  return false;
}

bool KateDocument::paintTextLine( QPainter &paint, uint line, int startcol, int endcol, int xPos2, int y, int xStart, int xEnd,
                                  int showCursor, bool replaceCursor, int cursorXPos2, bool showSelections, bool showTabs,
                                  WhichFont wf, bool currentLine, bool printerfriendly)
{
  // font data
  FontStruct *fs = (wf==ViewFont)?&viewFont:&printFont;

  // text attribs font/style data
  Attribute *at = myAttribs.data();
  uint atLen = myAttribs.size();

  // textline
  TextLine::Ptr textLine;

  // length, chars + raw attribs
  uint len = 0;
  const QChar *s;
  const uchar *a;

   // selection startcol/endcol calc
  bool hasSel = false;
  uint startSel = 0;
  uint endSel = 0;

  // was the selection background allready completly painted ?
  bool selectionPainted = false;

  // should the cursor be painted (if it is in the current xstart - xend range)
  bool cursorVisible = false;
  int cursorXPos = 0;
  int cursorMaxWidth = 0;
  QColor *cursorColor = 0;

  textLine = buffer->line(line);

  if (!textLine)
    return false;

  if (!textLine->isVisible())
    return false;

  len = textLine->length();
  uint oldLen = len;

  if (!printerfriendly && showSelections && lineSelected (line))
  {
    paint.fillRect(xPos2, y, xEnd - xStart, fs->fontHeight, colors[1]);
    selectionPainted = true;
    hasSel = true;
    startSel = 0;
    endSel = len + 1;
  }
  else if (!printerfriendly && currentLine)
    paint.fillRect(xPos2, y, xEnd - xStart, fs->fontHeight, colors[2]);
  else if (!printerfriendly)
    paint.fillRect(xPos2, y, xEnd - xStart, fs->fontHeight, colors[0]);

  if (startcol > (int)len)
    startcol = len;

  if (startcol < 0)
    startcol = 0;

  if (endcol < 0)
    len = len - startcol;
  else
    len = endcol - startcol;

  // text + attrib data from line
  s = textLine->text ();
  a = textLine->attributes ();

  // adjust to startcol ;)
  s = s + startcol;
  a = a + startcol;

  uint curCol = startcol;

  // or we will see no text ;)
  uint oldY = y;
  y += fs->fontAscent;

  // painting loop
  uint xPos = 0;
  uint xPosAfter = 0;
  uint width = 0;

  Attribute *curAt = 0;
  Attribute *oldAt = 0;

  QColor *curColor = 0;
  QColor *oldColor = 0;

  if (showSelections && !selectionPainted)
  {
    if (hasSelection())
    {
      if (!blockSelect)
      {
        if (((int)line == selectStart.line) && ((int)line < selectEnd.line))
        {
          startSel = selectStart.col;
          endSel = oldLen;
          hasSel = true;
        }
        else if (((int)line == selectEnd.line) && ((int)line > selectStart.line))
        {
          startSel = 0;
          endSel = selectEnd.col;
          hasSel = true;
        }
        else if (((int)line == selectEnd.line) && ((int)line == selectStart.line))
        {
          startSel = selectStart.col;
          endSel = selectEnd.col;
          hasSel = true;
        }
      }
      else
      {
        if (((int)line >= selectStart.line) && ((int)line <= selectEnd.line))
        {
          startSel = selectStart.col;
          endSel = selectEnd.col;
          hasSel = true;
        }
      }
    }
  }

  uint oldCol = startcol;
  uint oldXPos = xPos;
  const QChar *oldS = s;

  bool isSel = false;
  bool isTab = false;

  //kdDebug(13000)<<"paint 1"<<endl;

  if (len < 1)
  {
    //  kdDebug(13000)<<"paint 2"<<endl;

    if ((showCursor > -1) && (showCursor == (int)curCol))
    {
      cursorVisible = true;
      cursorXPos = xPos;
      cursorMaxWidth = xPosAfter - xPos;
      cursorColor = &at[0].col;
    }
    //  kdDebug(13000)<<"paint 3"<<endl;

  }
  else
    {
  for (uint tmp = len; (tmp > 0); tmp--)
  {
    // kdDebug(13000)<<"paint 4"<<endl;

    if ((*s) == QChar('\t'))
    {
      isTab = true;
      width = fs->m_tabWidth;
    }
    else
      isTab = false;

    if ((*a) >= atLen)
      curAt = &at[0];
    else
      curAt = &at[*a];

    if (curAt->bold && curAt->italic)
    {
      if (!isTab)
	width = fs->myFontMetricsBI.width(*s);

      if (curAt != oldAt)
        paint.setFont(fs->myFontBI);
    }
    else if (curAt->bold)
    {
      if (!isTab)
        width = fs->myFontMetricsBold.width(*s);

      if (curAt != oldAt)
        paint.setFont(fs->myFontBold);
    }
    else if (curAt->italic)
    {
      if (!isTab)
        width = fs->myFontMetricsItalic.width(*s);

      if (curAt != oldAt)
        paint.setFont(fs->myFontItalic);
    }
    else
    {
      if (!isTab)
        width = fs->myFontMetrics.width(*s);

      if (curAt != oldAt)
        paint.setFont(fs->myFont);
    }

    xPosAfter += width;

    //  kdDebug(13000)<<"paint 5"<<endl;

    if ((int)xPosAfter >= xStart)
    {
      curColor = &(curAt->col);
      isSel = false;

      if (showSelections && hasSel && (curCol >= startSel) && (curCol < endSel))
      {
      /*  if (!selectionPainted)
          paint.fillRect(xPos2 + xPos - xStart, oldY, xPosAfter - xPos, fs->fontHeight, colors[1]);*/

        curColor = &(curAt->selCol);
        isSel = true;
      }

      if (curColor != oldColor)
        paint.setPen(*curColor);

      // make sure we redraw the right character groups on attrib/selection changes
      if (isTab)
      {
        if (!printerfriendly && isSel && !selectionPainted)
          paint.fillRect(xPos2 + oldXPos - xStart, oldY, xPosAfter - oldXPos, fs->fontHeight, colors[1]);

        if (showTabs)
        {
          paint.drawPoint(xPos2 + xPos - xStart, y);
          paint.drawPoint(xPos2 + xPos - xStart + 1, y);
          paint.drawPoint(xPos2 + xPos - xStart, y - 1);
        }

        oldCol = curCol+1;
        oldXPos = xPosAfter;
        oldS = s+1;
      }
      else if (
           (tmp < 2) || ((int)xPos > xEnd) || (curAt != &at[*(a+1)]) ||
           (isSel != (hasSel && ((curCol+1) >= startSel) && ((curCol+1) < endSel))) ||
           (((*(s+1)) == QChar('\t')) && !isTab)
         )
      {
        if (!printerfriendly && isSel && !selectionPainted)
          paint.fillRect(xPos2 + oldXPos - xStart, oldY, xPosAfter - oldXPos, fs->fontHeight, colors[1]);

        QConstString str((QChar *) oldS, curCol+1-oldCol);
        paint.drawText(xPos2 + oldXPos-xStart, y, str.string());

        if ((int)xPos > xEnd)
          break;

        oldCol = curCol+1;
        oldXPos = xPosAfter;
        oldS = s+1;
      }


      if ((showCursor > -1) && (showCursor == (int)curCol))
      {
        cursorVisible = true;
        cursorXPos = xPos;
        cursorMaxWidth = xPosAfter - xPos;
        cursorColor = &curAt->col;
      }
    }
    else
    {
      oldCol = curCol+1;
      oldXPos = xPosAfter;
      oldS = s+1;
    }

    //   kdDebug(13000)<<"paint 6"<<endl;

    // increase xPos
    xPos = xPosAfter;

    // increase char + attribs pos
    s++;
    a++;

    // to only switch font/color if needed
    oldAt = curAt;
    oldColor = curColor;

    // col move
    curCol++;
  }
  // kdDebug(13000)<<"paint 7"<<endl;
  if ((showCursor > -1) && (showCursor == (int)curCol))
  {
    cursorVisible = true;
    cursorXPos = xPos;
    cursorMaxWidth = xPosAfter - xPos;
    cursorColor = &curAt->col;
  }
}
  //kdDebug(13000)<<"paint 8"<<endl;
  if (!printerfriendly && showSelections && !selectionPainted && lineEndSelected (line))
  {
    paint.fillRect(xPos2 + xPos-xStart, oldY, xEnd - xStart, fs->fontHeight, colors[1]);
    selectionPainted = true;
  }

  if (cursorVisible)
  {
    if (replaceCursor && (cursorMaxWidth > 2))
      paint.fillRect(xPos2 + cursorXPos-xStart, oldY, cursorMaxWidth, fs->fontHeight, *cursorColor);
    else
      paint.fillRect(xPos2 + cursorXPos-xStart, oldY, 2, fs->fontHeight, *cursorColor);
  }
  else if (showCursor > -1)
  {
    if ((cursorXPos2>=xStart) && (cursorXPos2<=xEnd))
    {                           
      cursorMaxWidth = fs->myFontMetrics.width(QChar (' '));
    
      if (replaceCursor && (cursorMaxWidth > 2))
        paint.fillRect(xPos2 + cursorXPos2-xStart, oldY, cursorMaxWidth, fs->fontHeight, myAttribs[0].col);
      else
        paint.fillRect(xPos2 + cursorXPos2-xStart, oldY, 2, fs->fontHeight, myAttribs[0].col);
    }
  }

  return true;
}

void KateDocument::newBracketMark( const KateTextCursor &cursor, BracketMark& bm )
{
  TextLine::Ptr textLine;
  int x, line, count, attr;
  QChar bracket, opposite, ch;
  Attribute *a;

  bm.eXPos = -1; //mark bracked mark as invalid
  x = cursor.col -1; // -1 to look at left side of cursor
  if (x < 0) return;
  line = cursor.line; //current line
  count = 0; //bracket counter for nested brackets

  textLine = buffer->line(line);
  if (!textLine) return;

  bracket = textLine->getChar(x);
  attr = textLine->attribute(x);

  if (bracket == '(' || bracket == '[' || bracket == '{')
  {
    //get opposite bracket
    opposite = ')';
    if (bracket == '[') opposite = ']';
    if (bracket == '{') opposite = '}';
    //get attribute of bracket (opposite bracket must have the same attribute)
    x++;
    while (line - cursor.line < 40) {
      //go to next line on end of line
      while (x >= (int) textLine->length()) {
        line++;
        if (line > (int) lastLine()) return;
        textLine = buffer->line(line);
        x = 0;
      }
      if (textLine->attribute(x) == attr) {
        //try to find opposite bracked
        ch = textLine->getChar(x);
        if (ch == bracket) count++; //same bracket : increase counter
        if (ch == opposite) {
          count--;
          if (count < 0) goto found;
        }
      }
      x++;
    }
  }
  else if (bracket == ')' || bracket == ']' || bracket == '}')
  {
    opposite = '(';
    if (bracket == ']') opposite = '[';
    if (bracket == '}') opposite = '{';
    x--;
    while (cursor.line - line < 20) {

      while (x < 0) {
        line--;
        if (line < 0) return;
        textLine = buffer->line(line);
        x = textLine->length() -1;
      }
      if (textLine->attribute(x) == attr) {
        ch = textLine->getChar(x);
        if (ch == bracket) count++;
        if (ch == opposite) {
          count--;
          if (count < 0) goto found;
        }
      }
      x--;
    }
  }
  return;

found:
  //cursor position of opposite bracket
  bm.cursor.col = x;
  bm.cursor.line = line;
  //x position (start and end) of related bracket
  bm.sXPos = textWidth(textLine, x);
  a = attribute(attr);

   if (a->bold && a->italic)
      bm.eXPos = bm.sXPos + viewFont.myFontMetricsBI.width(bracket);
    else if (a->bold)
      bm.eXPos = bm.sXPos + viewFont.myFontMetricsBold.width(bracket);
    else if (a->italic)
      bm.eXPos = bm.sXPos + viewFont.myFontMetricsItalic.width(bracket);
    else
      bm.eXPos = bm.sXPos + viewFont.myFontMetrics.width(bracket);
}

void KateDocument::guiActivateEvent( KParts::GUIActivateEvent *ev )
{
  KParts::ReadWritePart::guiActivateEvent( ev );
  if ( ev->activated() )
    emit selectionChanged();
}

void KateDocument::setDocName (QString docName)
{
  myDocName = docName;
  emit nameChanged ((Kate::Document *) this);
}

void KateDocument::setMTime()
{
    if (fileInfo && !fileInfo->fileName().isEmpty()) {
      fileInfo->refresh();
      mTime = fileInfo->lastModified();
    }
}

void KateDocument::isModOnHD(bool forceReload)
{
  if (fileInfo && !fileInfo->fileName().isEmpty()) {
    fileInfo->refresh();
    if (fileInfo->lastModified() > mTime) {
      if ( forceReload ||
           (KMessageBox::warningContinueCancel(0,
               (i18n("The file %1 has changed on disk.\nDo you want to reload the modified file?\n\nIf you choose Cancel and subsequently save the file, you will lose those modifications.")).arg(url().filename()),
               i18n("File has Changed on Disk"),
               i18n("Yes") ) == KMessageBox::Continue)
          )
        reloadFile();
      else
        setMTime();
    }
  }
}

void KateDocument::reloadFile()
{
  if (fileInfo && !fileInfo->fileName().isEmpty())
  {
    uint mode = hlMode ();
    bool byUser = hlSetByUser;

    KateDocument::openURL( url() );
    setMTime();
    
    if (byUser)
      setHlMode (mode);
  }
}

void KateDocument::slotModChanged()
{
  emit modStateChanged ((Kate::Document *)this);
}

void KateDocument::flush ()
{
  if (!isReadWrite())
    return;

  m_url = KURL();
  fileInfo->setFile (QString());
  setMTime();

  clear();
  updateViews();

  emit fileNameChanged ();
}

void KateDocument::open (const QString &name)
{
  openURL (KURL (name));
}

Attribute *KateDocument::attribute (uint pos)
{
  if (pos < myAttribs.size())
    return &myAttribs[pos];

  return &myAttribs[0];
}

void KateDocument::wrapText (uint col)
{
  wrapText (0, lastLine(), col);
}

void KateDocument::setWordWrap (bool on)
{
  if (on != myWordWrap && on)
    wrapText (myWordWrapAt);

  myWordWrap = on;
}

void KateDocument::setWordWrapAt (uint col)
{
  if (myWordWrapAt != col && myWordWrap)
    wrapText (myWordWrapAt);

  myWordWrapAt = col;
}

void KateDocument::applyWordWrap ()
{
  wrapText (myWordWrapAt);
}

uint KateDocument::configFlags ()
{
  return _configFlags;
}

void KateDocument::setConfigFlags (uint flags)
{
  bool updateView;

  if (flags != _configFlags)
  {
    // update the view if visibility of tabs has changed
    updateView = (flags ^ _configFlags) & KateDocument::cfShowTabs;
    _configFlags = flags;
    //emit newStatus();
    if (updateView) updateViews ();
  }
}



void KateDocument::exportAs(const QString& filter)
{
  if (filter=="kate_html_export")
  {
    QString filename=KFileDialog::getSaveFileName(QString::null,QString::null,0,i18n("Export File As"));
    if (filename.isEmpty())
      {
        return;
      }
    KSaveFile *savefile=new KSaveFile(filename);
    if (!savefile->status())
    {
      if (exportDocumentToHTML(savefile->textStream(),filename)) savefile->close(); 
        else savefile->abort();
      //if (!savefile->status()) --> Error
    } else {/*ERROR*/}
    delete savefile;
  }
}

/* For now, this should become an plugin */
bool KateDocument::exportDocumentToHTML(QTextStream *outputStream,const QString &name)
{
  outputStream->setEncoding(QTextStream::UnicodeUTF8);
  // let's write the HTML header :
  (*outputStream) << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>" << endl;
  (*outputStream) << "<!DOCTYPE html PUBLIC \"-//W3C//DTD XHTML 1.0 Strict//EN\" \"DTD/xhtml1-strict.dtd\">" << endl;
  (*outputStream) << "<html xmlns=\"http://www.w3.org/1999/xhtml\">" << endl;
  (*outputStream) << "<head>" << endl;
  (*outputStream) << "<meta http-equiv=\"Content-Type\" content=\"text/html; charset=UTF-8\" />" << endl;
  (*outputStream) << "<meta name=\"Generator\" content=\"Kate, the KDE Advanced Text Editor\" />" << endl;
  // for the title, we write the name of the file (/usr/local/emmanuel/myfile.cpp -> myfile.cpp)
  (*outputStream) << "<title>" << name.right(name.length() - name.findRev('/') -1) << "</title>" << endl;
  (*outputStream) << "</head>" << endl;

  (*outputStream) << "<body><pre>" << endl;
  // for each line :

  // some variables :
  bool previousCharacterWasBold = false;
  bool previousCharacterWasItalic = false;
  // when entering a new color, we'll close all the <b> & <i> tags,
  // for HTML compliancy. that means right after that font tag, we'll
  // need to reinitialize the <b> and <i> tags.
  bool needToReinitializeTags = false;
  QColor previousCharacterColor(0,0,0); // default color of HTML characters is black
  (*outputStream) << "<span style='color=#000000'>";

  for (uint curLine=0;curLine<numLines();curLine++)
  { // html-export that line :
    TextLine::Ptr textLine = buffer->line(curLine);
    //ASSERT(textLine != NULL);
    // for each character of the line : (curPos is the position in the line)
    for (uint curPos=0;curPos<textLine->length();curPos++)
    {
      Attribute *charAttributes = attribute(textLine->attribute(curPos));
      //ASSERT(charAttributes != NULL);
      // let's give the color for that character :
      if ( (charAttributes->col != previousCharacterColor))
      {  // the new character has a different color :
        // if we were in a bold or italic section, close it
        if (previousCharacterWasBold)
          (*outputStream) << "</b>";
        if (previousCharacterWasItalic)
          (*outputStream) << "</i>";

        // close the previous font tag :
        (*outputStream) << "</span>";
        // let's read that color :
        int red, green, blue;
        // getting the red, green, blue values of the color :
        charAttributes->col.rgb(&red, &green, &blue);
        (*outputStream) << "<span style='color:#"
              << ( (red < 0x10)?"0":"")  // need to put 0f, NOT f for instance. don't touch 1f.
              << QString::number(red, 16) // html wants the hex value here (hence the 16)
              << ( (green < 0x10)?"0":"")
              << QString::number(green, 16)
              << ( (blue < 0x10)?"0":"")
              << QString::number(blue, 16)
              << "'>";
        // we need to reinitialize the bold/italic status, since we closed all the tags
        needToReinitializeTags = true;
      }
      // bold status :
      if ( (needToReinitializeTags && charAttributes->bold) ||
          (!previousCharacterWasBold && charAttributes->bold) )
        // we enter a bold section
        (*outputStream) << "<b>";
      if ( !needToReinitializeTags && (previousCharacterWasBold && !charAttributes->bold) )
        // we leave a bold section
        (*outputStream) << "</b>";

      // italic status :
      if ( (needToReinitializeTags && charAttributes->italic) ||
           (!previousCharacterWasItalic && charAttributes->italic) )
        // we enter an italic section
        (*outputStream) << "<i>";
      if ( !needToReinitializeTags && (previousCharacterWasItalic && !charAttributes->italic) )
        // we leave an italic section
        (*outputStream) << "</i>";

      // write the actual character :
      (*outputStream) << HTMLEncode(textLine->getChar(curPos));

      // save status for the next character :
      previousCharacterWasItalic = charAttributes->italic;
      previousCharacterWasBold = charAttributes->bold;
      previousCharacterColor = charAttributes->col;
      needToReinitializeTags = false;
    }
    // finish the line :
    (*outputStream) << endl;
  }
  // HTML document end :
  (*outputStream) << "</span>";  // i'm guaranteed a span is started (i started one at the beginning of the output).
  (*outputStream) << "</pre></body>";
  (*outputStream) << "</html>";
  // close the file :
  return true;
}

QString KateDocument::HTMLEncode(QChar theChar)
{
  switch (theChar.latin1())
  {
  case '>':
    return QString("&gt;");
  case '<':
    return QString("&lt;");
  case '&':
    return QString("&amp;");
  };
  return theChar;
}

Kate::ConfigPage *KateDocument::colorConfigPage (QWidget *p)
{
  return (Kate::ConfigPage*) new ColorConfig(p, "", this);
}

Kate::ConfigPage *KateDocument::fontConfigPage (QWidget *p)
{
  return (Kate::ConfigPage*) new FontConfig(p, "", this);
}

Kate::ConfigPage *KateDocument::indentConfigPage (QWidget *p)
{
  return (Kate::ConfigPage*) new IndentConfigTab(p, this);
}

Kate::ConfigPage *KateDocument::selectConfigPage (QWidget *p)
{
  return (Kate::ConfigPage*) new SelectConfigTab(p, this);
}

Kate::ConfigPage *KateDocument::editConfigPage (QWidget *p)
{
  return (Kate::ConfigPage*) new EditConfigTab(p, this);
}

Kate::ConfigPage *KateDocument::keysConfigPage (QWidget *p)
{
  return (Kate::ConfigPage*) new EditKeyConfiguration(p);
}

Kate::ConfigPage *KateDocument::hlConfigPage (QWidget *p)
{
  return (Kate::ConfigPage*) new HlConfigPage (p, this);
}

Kate::ActionMenu *KateDocument::hlActionMenu (const QString& text, QObject* parent, const char* name)
{
  KateViewHighlightAction *menu = new KateViewHighlightAction (text, parent, name);
  menu->updateMenu (this);

  return (Kate::ActionMenu *)menu;
}

Kate::ActionMenu *KateDocument::exportActionMenu (const QString& text, QObject* parent, const char* name)
{
  KateExportAction *menu = new KateExportAction (text, parent, name);
  menu->updateMenu (this);

  return (Kate::ActionMenu *)menu;
}


void KateDocument::dumpRegionTree()
{
  regionTree->debugDump();
}

unsigned int KateDocument::getRealLine(unsigned int virtualLine)
{
  return regionTree->getRealLine(virtualLine);
}

unsigned int KateDocument::getVirtualLine(unsigned int realLine)
{
  return regionTree->getVirtualLine(realLine);
}

unsigned int KateDocument::visibleLines ()
{
  return numLines() - regionTree->getHiddenLinesCount();
}

void KateDocument::slotLoadingFinished()
{
  tagAll();
}

/**
 * Begin of the implementaion of the MarkInterfaceExtension
 **/

void KateDocument::setPixmap(MarkInterface::MarkTypes, const QPixmap &)
{
  ;
}
void KateDocument::setDescription(MarkInterface::MarkTypes, const QString &)
{
  ;
}

void KateDocument::setMarksUserChangable(uint markMask)
{
  m_editableMarks=markMask;
}

// -----------------------
uint KateDocument::editableMarks()
{
  return m_editableMarks;
}

/**
 * End of the implementaion of the MarkInterfaceExtension
 **/



QFont KateDocument::getFont (WhichFont wf) { if(wf==ViewFont) return viewFont.myFont; else return printFont.myFont;}

KateFontMetrics KateDocument::getFontMetrics (WhichFont wf) { if (wf==ViewFont) return viewFont.myFontMetrics; else return printFont.myFontMetrics;}

TextLine::Ptr KateDocument::kateTextLine(uint i)
{
  return buffer->line (i);
}

//
// KTextEditor::CursorInterface stuff
//
KTextEditor::Cursor *KateDocument::createCursor ( )
{
  return new KateCursor (this);
}




