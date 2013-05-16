# -*- codincmakeToolViewg: utf-8 -*-
'''CMake helper plugin'''

#
# CMake helper plugin
#
# Copyright 2013 by Alex Turbov <i.zaufi@gmail.com>
#
# This software is free software: you can redistribute it and/or modify
# it under the terms of the GNU Lesser General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This software is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public License
# along with this software.  If not, see <http://www.gnu.org/licenses/>.


import imp
import functools
import glob
import os
import re
import subprocess
import sys
import types

from PyQt4 import uic
from PyQt4.QtCore import QEvent, QObject, QUrl, Qt, pyqtSlot
from PyQt4.QtGui import (
    QBrush
  , QCheckBox
  , QColor
  , QSizePolicy
  , QSpacerItem
  , QSplitter
  , QTabWidget
  , QTextBrowser
  , QTreeWidget
  , QTreeWidgetItem
  , QVBoxLayout
  , QWidget
  )

from PyKDE4.kdecore import i18nc, KUrl
from PyKDE4.kio import KFile, KFileDialog, KUrlRequesterDialog
from PyKDE4.ktexteditor import KTextEditor

import kate

from libkatepate import ui, common
from libkatepate.autocomplete import AbstractCodeCompletionModel

from cmake_utils_settings import *
import cmake_help_parser


cmakeToolView = None

# ----------------------------------------------------------
# CMake utils: open CMakeLists.txt action
# ----------------------------------------------------------

_CMAKE_LISTS = 'CMakeLists.txt'
_CMAKE_MIME_TYPE = 'text/x-cmake'

_ADD_SUB_RE = re.compile('^[^#]*add_subdirectory\((.*)\).*$')
_SUBDIR_OPT = 'EXCLUDE_FROM_ALL'


def _get_1st_arg_if_add_subdirectory_cmd(document, line):
    # TODO WTF? '\badd_subdirectory' doesn't match! Just wanted to
    # make sure that 'add_subdirectory' is not a part of some other word...
    match = _ADD_SUB_RE.search(document.line(line))
    if match:
        print('CMakeHelper: line[{}] "{}" match: {}'.format(line, document.line(line), match.groups()))
        return [subdir for subdir in match.group(1).split() if subdir != _SUBDIR_OPT][0]
    return None


def _find_current_context(document, cursor):
    '''Determinate current context under cursor'''
    # Parse whole document starting from a very first line!
    in_a_string = False
    in_a_command = False
    in_a_comment = False
    skip_next = False
    nested_var_level = 0
    command = None
    fn_params_start = None
    for current_line in range(0, cursor.line() + 1):
        line_str = document.line(current_line)
        prev = None
        in_a_comment = False
        should_count_pos = (current_line == cursor.line())
        for pos, c in enumerate(line_str):
            if should_count_pos and pos == cursor.column():
                break
            if c == '#' and not in_a_string:
                in_a_comment = True
                # TODO Syntax error if we r in a var expansion
                break                                       # Ignore everything till the end of line
            if skip_next:                                   # Should we skip current char?
                skip_next = False                           # Yep!
            elif c == '\\':                                 # Found a backslash:
                skip_next = True                            #  skip next char!
            elif c == '"':                                  # Found a quote char
                in_a_string = not in_a_string               # Switch 'in a string' state
                # TODO Syntax error if we r in a var expansion
            elif c == '{' and prev == '$':                  # Looks like a variable expansion?
                nested_var_level += 1                       # Yep, incrase var level
            elif c == '}':                                  # End of a variable expansion
                nested_var_level -= 1
            elif c == '(' and not in_a_string:              # Command params started
                command = line_str[0:pos].strip()
                # TODO Syntax error if we r in a var expansion
                in_a_command = True
                fn_params_start = KTextEditor.Cursor(current_line, pos + 1)
            elif c == ')' and not in_a_string:
                # TODO Syntax error if we r in a var expansion
                in_a_command = False
                command = None
                fn_params_start = None

            # TODO Handle generator expressions

            # Remember current char in a `prev' for next iteration
            prev = c
    if fn_params_start is not None:
        fn_params_range = KTextEditor.Range(fn_params_start, cursor)
    else:
        fn_params_range = KTextEditor.Range(-1, -1, -1, -1)
    return (command, in_a_string, nested_var_level != 0, in_a_comment, fn_params_range)


def _is_there_CMakeLists(path):
    '''Try to find `CMakeLists.txt` in a given path'''
    print('CMakeHelper: checking `{}` for CMakeLists.txt'.format(path))
    assert(isinstance(path, str))
    if os.access(os.path.join(path, _CMAKE_LISTS), os.R_OK):
        return True
    return False


def _find_CMakeLists(start_dir, parent_cnt = 0):
    '''Try to find `CMakeLists.txt` startring form a given path'''
    if _is_there_CMakeLists(start_dir):
        return os.path.join(start_dir, _CMAKE_LISTS)
    if parent_cnt < kate.configuration[PARENT_DIRS_LOOKUP_CNT]:
        return _find_CMakeLists(os.path.dirname(start_dir), parent_cnt + 1)
    return None


def _openDocumentNoCheck(url):
    document = kate.documentManager.openUrl(KUrl(url))
    document.setReadWrite(os.access(url.toLocalFile(), os.W_OK))
    kate.application.activeMainWindow().activateView(document)


@pyqtSlot(QUrl)
def openDocument(url):
    local_file = url.toLocalFile()
    print('CMakeCC: going to open the document: {}'.format(local_file))
    if os.access(local_file, os.R_OK):
        _openDocumentNoCheck(url)
    else:
        ui.popup(
            i18nc('@title:window', 'Error')
          , i18nc('@info:tooltip', 'Unable to open the document: <filename>%1</filename>', local_file)
          , 'dialog-error'
          )


def _ask_for_CMakeLists_location_and_try_open(start_dir_to_show, cur_doc_dir):
    selected_dir = KUrlRequesterDialog.getUrl(
        start_dir_to_show
      , kate.mainInterfaceWindow().window()
      , i18nc('@title:window', '<filename>CMakeLists.txt</filename> location')
      )
    print('CMakeHelper: selected_dir={}'.format(selected_dir))

    if selected_dir.isEmpty():
        return                                              # User pressed 'Cancel'

    selected_dir = selected_dir.toLocalFile()               # Get selected path
    # Is it relative?
    if not os.path.isabs(selected_dir):
        # Yep, join w/ a path of the current doc
        selected_dir = os.path.abspath(os.path.join(cur_doc_dir, selected_dir))

    # Check if there CMakeLists.txt present
    cmakelists = os.path.join(selected_dir, _CMAKE_LISTS)
    if _is_there_CMakeLists(selected_dir):
        # Open it!
        _openDocumentNoCheck(QUrl.fromLocalFile(cmakelists))
    else:
        ui.popup(
            i18nc('@title:window', 'Error')
          , i18nc('@info:tooltip', 'No such file <filename>%1</filename>', cmakelists)
          , 'dialog-error'
          )


def _try_open_CMakeLists(start_dir):
    '''Check if given dir has CMakeLists.txt, try to open it'''
    if _is_there_CMakeLists(start_dir):
        _openDocumentNoCheck(QUrl.fromLocalFile(os.path.join(start_dir, _CMAKE_LISTS)))
        return True
    return False


@kate.action(i18nc('@action:inmenu', 'Open CMakeLists...'), menu='Tools', shortcut='Meta+M')
def openCMakeList():
    # TODO Handle unnamed docs
    # First of all check the document's MIME-type
    document = kate.activeDocument()
    cur_dir = os.path.dirname(document.url().toLocalFile())
    mimetype = document.mimeType()
    if mimetype != _CMAKE_MIME_TYPE:
        # Ok, current document is not a CMakeLists.txt, lets try to open
        # it in a current directory
        if not _try_open_CMakeLists(cur_dir):
            # Ok, no CMakeLists.txt found, lets ask a user
            _ask_for_CMakeLists_location_and_try_open(cur_dir, cur_dir)
        return

    # Ok, we are in some CMake module...
    # Check if there smth selected by user?
    view = kate.activeView()
    selectedRange = view.selectionRange()
    if not selectedRange.isEmpty():
        selected_dir = document.text(selectedRange)
    else:
        # Ok, nothing selected. Lets check the context: are we inside a command?
        cursor = view.cursorPosition()
        command, in_a_string, in_a_var, in_a_comment, fn_params_range = _find_current_context(document, cursor)
        print('CMakeHelper: command="{}", in_a_string={}, in_a_var={}'.format(command, in_a_string, in_a_var))
        selected_dir = cur_dir
        if command == 'add_subdirectory':
            # Check if the command have some parameters already entered
            if fn_params_range.isValid():
                # Ok, get the arg right under cursor
                wordRange = common.getBoundTextRangeSL(' \n()', ' \n()', cursor, document)
                selected_dir = document.text(wordRange)
                print('CMakeHelper: word@cursor={}'.format(selected_dir))
        # TODO Try to handle cursor pointed in `include()` command?
        else:
            # Huh, lets find a nearest add_subdirectory command
            # and get its first arg
            if cursor.line() == 0:
                back_line = -1
                fwd_line = cursor.line()
            elif cursor.line() == (document.lines() - 1):
                back_line = cursor.line()
                fwd_line = document.lines()
            else:
                back_line = cursor.line()
                fwd_line = cursor.line() + 1
            found = None
            while not found and (back_line != -1 or fwd_line != document.lines()):
                # Try to match a line towards a document's start
                if 0 <= back_line:
                    found = _get_1st_arg_if_add_subdirectory_cmd(document, back_line)
                    back_line -= 1
                # Try to match a line towards a document's end
                if not found and fwd_line < document.lines():
                    found = _get_1st_arg_if_add_subdirectory_cmd(document, fwd_line)
                    fwd_line += 1
            # Let to user specify anything he wants.
            if found:
                selected_dir = found
            _ask_for_CMakeLists_location_and_try_open(selected_dir, cur_dir)
            return

    if not os.path.isabs(selected_dir):
        selected_dir = os.path.join(cur_dir, selected_dir)

    # Try to open
    if not _try_open_CMakeLists(selected_dir):
        # Yep, show a location dialog w/ an initial dir equal to a selected text
        _ask_for_CMakeLists_location_and_try_open(selected_dir, cur_dir)

# ----------------------------------------------------------
# CMake utils: completion stuff
# ----------------------------------------------------------

class CMakeCompletionModel(AbstractCodeCompletionModel):
    '''Completion model for CMake files'''
    # TODO Unit tests

    TITLE_AUTOCOMPLETION = i18nc('@label:listbox', 'CMake Auto Completion')
    MAX_DESCRIPTION = 100
    GROUP_POSITION = AbstractCodeCompletionModel.GroupPosition.BEST_MATCHES

    _cc_registrar_fn_name = 'register_command_completer'

    def __init__(self, app):
        super(CMakeCompletionModel, self).__init__(app)
        # Create an empty dict of command completers
        self.__command_completers = {}
        # Try to load all available particular command completers
        self._loadCompleters()

    def reset(self):
        '''Reset the model'''
        self.resultList = []

    def completionInvoked(self, view, word, invocationType):
        '''Completion has been inviked for given view and word'''

        self.reset()                                        # Drop previously collected completions

        # First of all check the document's MIME-type
        document = view.document()
        mimetype = document.mimeType()

        if mimetype != _CMAKE_MIME_TYPE:
            return

        print('CMakeCC [{}]: current word: "{}"'.format(mimetype, word))

        cursor = view.cursorPosition()
        # Try to detect completion context
        command, in_a_string, in_a_var, in_a_comment, fn_params_range = _find_current_context(document, cursor)
        print(
            'CMakeCC: command="{}", in_a_string={}, in_a_var={}, in_a_comment={}'.
            format(command, in_a_string, in_a_var, in_a_comment)
          )
        if fn_params_range.isValid():
            print('CMakeCC: params="{}"'.format(document.text(fn_params_range)))

        if in_a_comment:
            # Nothing to complete if we r in a comment
            return

        if in_a_var:
            # Try to complete a variable name
            self.TITLE_AUTOCOMPLETION = i18nc('@label:listbox', 'CMake Variables Completion')
            for var in cmake_help_parser.get_cmake_vars():
                self.resultList.append(
                    self.createItemAutoComplete(
                        text=var[0]
                      , category='constant'
                      , description=var[1]
                      )
                  )
            return
        if in_a_string:
            # If we a not in a variable expansion, but inside a string
            # there is nothing to complete!
            return
        # Try to complete a command
        if not command:
            # Try to complete a variable name
            self.TITLE_AUTOCOMPLETION = i18nc('@label:listbox', 'CMake Commands Completion')
            for cmd in cmake_help_parser.get_cmake_commands():
                self.resultList.append(
                    self.createItemAutoComplete(
                        text=cmd[0]
                      , category='function'
                      , description=cmd[1]
                      )
                  )
            return
        # Try to complete a particular command
        # 0) assemble parameter list preceded to the current completion position
        # TODO Unit tests
        text = document.text(fn_params_range).strip().split()
        found_string = False
        comp_list = []
        for tok in text:
            if tok.startswith('"'):
                comp_list.append(tok)
                found_string = True
            elif tok.endswith('"'):
                comp_list[-1] += ' ' + tok
                found_string = False
            elif found_string:
                comp_list[-1] += ' ' + tok
            else:
                comp_list.append(tok)
        # 1) call command completer
        self.try_complete_command(command, document, cursor, word, comp_list, invocationType)


    def executeCompletionItem(self, document, word, row):
        # TODO Why this method is not called???
        print('CMakeCC: executeCompletionItem: ' + repr(word)+', row='+str(row))


    def _loadCompleters(self):
        # Load available command completers
        for directory in kate.applicationDirectories('cmake_utils/command_completers'):
            print('CMakeCC: directory={}'.format(directory))
            sys.path.append(directory)
            for completer in glob.glob(os.path.join(directory, '*_cc.py')):
                print('CMakeCC: completer={}'.format(completer))
                cc_name = os.path.basename(completer).split('.')[0]
                module = imp.load_source(cc_name, completer)
                if hasattr(module, self._cc_registrar_fn_name):
                    r = getattr(module, self._cc_registrar_fn_name)
                    if isinstance(r, types.FunctionType):
                        r(self.__command_completers)


    def has_completion_for_command(self, command):
        return command in self.__command_completers


    def try_complete_command(self, command, document, cursor, word, comp_list, invocationType):
        '''Try to complete a command'''
        if self.has_completion_for_command(command):
            if isinstance(self.__command_completers[command], types.FunctionType):
                # If a function registered as a completer, just call it...
                completions = self.__command_completers[command](document, cursor, word, comp_list)
            else:
                # Everything else, that is not a function, just pass to the generic completer
                completions = self._try_syntactic_completer(
                    self.__command_completers[command]
                  , document
                  , cursor
                  , word
                  , comp_list
                  )
        else:
            if invocationType != KTextEditor.CodeCompletionModel.AutomaticInvocation:
                # Show popup only if user explictly requested code completion
                ui.popup(
                    i18nc('@title:window', 'Attention')
                  , i18nc('@info:tooltip', 'Sorry, no completion for <command>%1()</command>', command)
                  , 'dialog-information'
                  )

            completions = []

        # Result of a completion function must be a list type
        if completions and isinstance(completions, list):
            self.TITLE_AUTOCOMPLETION = i18nc(
                '@label:listbox'
              , 'CMake <command>%1()</command> Completion', command
              )
            for c in completions:
                # If completion item is a tuple, we expect to have 2 items in it:
                # 0 is a 'text' and 1 is a 'description'
                if isinstance(c, tuple) and len(c) == 2:
                    self.resultList.append(
                        self.createItemAutoComplete(
                            text=c[0]
                          , description=c[1]
                          )
                      )
                else:
                    self.resultList.append(self.createItemAutoComplete(text=c))


    def _try_syntactic_completer(self, syntax, document, cursor, word, comp_list):
        print('CMakeCC: generic completer: syntax='+str(syntax))
        print('CMakeCC: generic completer: comp_list='+str(comp_list))
        result = []
        if isinstance(syntax, list):
            for sid, s in enumerate(syntax):
                (items, stop) = s.complete(document, cursor, word, comp_list, sid)
                if stop:
                    return items
                result += items
        else:
            (items, stop) = syntax.complete(document, cursor, word, comp_list)
            result = items
        print('CMakeCC: generic completer result={}'.format(result))
        # TODO sort | uniq
        return result


def _reset(*args, **kwargs):
    cmake_completion_model.reset()


# ----------------------------------------------------------
# CMake utils: toolview
# ----------------------------------------------------------

class CMakeToolView(QObject):
    '''CMake tool view class

        TODO Remember last dir/position/is-advanced?
        TODO Make the cache view editable and run `cmake` to reconfigure
    '''
    cmakeCache = []

    def __init__(self, parent):
        super(CMakeToolView, self).__init__(parent)
        self.toolView = kate.mainInterfaceWindow().createToolView(
            'cmake_utils'
          , kate.Kate.MainWindow.Bottom
          , kate.gui.loadIcon('cmake')
          , i18nc('@title:tab', 'CMake')
          )
        self.toolView.installEventFilter(self)
        # By default, the toolview has box layout, which is not easy to delete.
        # For now, just add an extra widget.
        tabs = QTabWidget(self.toolView)
        # Make a page to view cmake cache
        self.cacheViewPage = uic.loadUi(
            os.path.join(os.path.dirname(__file__), CMAKE_TOOLVIEW_CACHEVIEW_UI)
          )
        self.cacheViewPage.buildDir.setText(kate.configuration[PROJECT_DIR])
        # TODO It seems not only KTextEditor's SIP files are damn out of date...
        # KUrlRequester actually *HAS* setPlaceholderText() method... but damn SIP
        # files for KIO are damn out of date either! A NEW BUG NEEDS TO BE ADDED!
        # (but I have fraking doubts that it will be closed next few damn years)
        #
        #self.buildDir.setPlaceholderText(i18nc('@info', 'Project build directory'))
        self.cacheViewPage.buildDir.lineEdit().setPlaceholderText(
            i18nc('@info/plain', 'Project build directory')
          )
        self.cacheViewPage.buildDir.setMode(
            KFile.Mode(KFile.Directory | KFile.ExistingOnly | KFile.LocalOnly)
          )
        self.cacheViewPage.cacheItems.sortItems(0, Qt.AscendingOrder)
        self.cacheViewPage.cacheFilter.setTreeWidget(self.cacheViewPage.cacheItems)
        tabs.addTab(self.cacheViewPage, i18nc('@title:tab', 'CMake Cache Viewer'))
        # Make a page w/ cmake help
        splitter = QSplitter(Qt.Horizontal, tabs)
        self.vewHelpPage = uic.loadUi(
            os.path.join(os.path.dirname(__file__), CMAKE_TOOLVIEW_HELP_UI)
          )
        self.vewHelpPage.helpFilter.setTreeWidget(self.vewHelpPage.helpTargets)
        self.updateHelpIndex()                              # Prepare Help view
        self.helpPage = QTextBrowser(splitter)
        self.helpPage.setReadOnly(True)
        self.helpPage.setOpenExternalLinks(False)
        self.helpPage.setOpenLinks(False)
        splitter.addWidget(self.vewHelpPage)
        splitter.addWidget(self.helpPage)
        splitter.setStretchFactor(0, 10)
        splitter.setStretchFactor(1, 20)
        tabs.addTab(splitter, i18nc('@title:tab', 'CMake Help'))
        # Make a page w/ some instant settings
        self.cfgPage = uic.loadUi(
            os.path.join(os.path.dirname(__file__), CMAKE_TOOLVIEW_SETTINGS_UI)
          )
        self.cfgPage.mode.setChecked(kate.configuration[TOOLVIEW_ADVANCED_MODE])
        self.cfgPage.htmlize.setChecked(kate.configuration[TOOLVIEW_BEAUTIFY])
        tabs.addTab(self.cfgPage, i18nc('@title:tab', 'Toolview Settings'))

        # Connect signals
        self.cacheViewPage.cacheItems.itemActivated.connect(self.insertIntoCurrentDocument)
        self.cacheViewPage.buildDir.returnPressed.connect(self.updateCacheView)
        self.cacheViewPage.buildDir.urlSelected.connect(self.updateCacheView)
        self.cfgPage.mode.toggled.connect(self.updateCacheView)
        self.cfgPage.mode.toggled.connect(self.saveSettings)
        self.cfgPage.htmlize.toggled.connect(self.updateHelpText)
        self.cfgPage.htmlize.toggled.connect(self.saveSettings)
        self.vewHelpPage.helpTargets.itemActivated.connect(self.updateHelpText)
        self.vewHelpPage.helpTargets.itemDoubleClicked.connect(self.insertHelpItemIntoCurrentDocument)
        self.helpPage.anchorClicked.connect(openDocument)

        # Refresh the cache view
        self._updateCacheView(self.cacheViewPage.buildDir.text())


    def __del__(self):
        """Plugins that use a toolview need to delete it for reloading to work."""
        if self.toolView:
            self.toolView.deleteLater()
            self.toolView = None


    def eventFilter(self, obj, event):
        """Hide the Palette tool view on ESCAPE key"""
        if event.type() == QEvent.KeyPress and event.key() == Qt.Key_Escape:
            kate.mainInterfaceWindow().hideToolView(self.toolView)
            return True
        return self.toolView.eventFilter(obj, event)


    @pyqtSlot()
    def saveSettings(self):
        kate.configuration[TOOLVIEW_ADVANCED_MODE] = self.cfgPage.mode.isChecked()
        kate.configuration[TOOLVIEW_BEAUTIFY] = self.cfgPage.htmlize.isChecked()


    @pyqtSlot()
    def updateCacheView(self):
        self._updateCacheView(self.cacheViewPage.buildDir.text())


    def _updateCacheView(self, build_dir):
        # Do nothing if build dir is not configured
        if not build_dir:
            return

        self.cacheViewPage.cacheItems.clear()               # Remove previously collected cache
        is_advanced = self.cfgPage.mode.isChecked()

        try:
            items = cmake_help_parser.get_cache_content(build_dir, is_advanced)
            print('CMakeCC: update cache view: result={}'.format(items))
        except ValueError as error:
            ui.popup(
                i18nc('@title:window', 'Error')
              , i18nc('@info:tooltip', 'Unable to get CMake cache content:<nl/><message>%1</message>', error)
              , 'dialog-error'
              )
            return

        # Add items to a list
        for key, value in items.items():
            item = QTreeWidgetItem(self.cacheViewPage.cacheItems, [key, value[1], value[0]])
            item.setToolTip(0, value[2])

        self.cacheViewPage.cacheItems.resizeColumnToContents(0)
        self.cacheViewPage.cacheItems.resizeColumnToContents(1)
        self.cacheViewPage.cacheItems.resizeColumnToContents(2)


    def updateHelpIndex(self):
        # Add commands group
        commands = QTreeWidgetItem(
            self.vewHelpPage.helpTargets
          , [i18nc('@item::inlistbox/plain', 'Commands')]
          , cmake_help_parser.help_category.COMMAND
          )
        deprecated = [cmd[0] for cmd in cmake_help_parser.get_cmake_deprecated_commands_list()]
        for cmd in cmake_help_parser.get_cmake_commands_list():
            c = QTreeWidgetItem(
                commands
              , [cmd]
              , cmake_help_parser.help_category.HELP_ITEM
              )
            global cmake_completion_model
            if cmake_completion_model.has_completion_for_command(cmd):
                c.setForeground(0, QBrush(QColor(Qt.green)))
            else:
                if cmd in deprecated:
                    c.setForeground(0, QBrush(QColor(Qt.yellow)))
                else:
                    c.setForeground(0, QBrush(QColor(Qt.red)))

        # Add modules group
        standard_modules = cmake_help_parser.get_cmake_modules_list()
        total_modules_count = len(standard_modules)
        custom_modules = {}
        for path in kate.configuration[AUX_MODULE_DIRS]:
            modules_list = cmake_help_parser.get_cmake_modules_list(path)
            filtered_modules_list = [mod for mod in modules_list if mod not in standard_modules]
            filtered_modules_list.sort()
            custom_modules[
                i18nc('@item:inlistbox', 'Modules from %1 (%2)', path, len(path))
              ] = filtered_modules_list
            total_modules_count += len(filtered_modules_list)
        custom_modules[
            i18nc('@item:inlistbox', 'Standard modules (%1)', len(standard_modules))
          ] = standard_modules
        #
        modules = QTreeWidgetItem(
            self.vewHelpPage.helpTargets
          , [i18nc('@item::inlistbox/plain', 'Modules (%1)', total_modules_count)]
          , cmake_help_parser.help_category.MODULE
          )
        for from_path, modules_list in custom_modules.items():
            ss_item = QTreeWidgetItem(
                modules
              , [from_path]
              , cmake_help_parser.help_category.MODULE
              )
            for mod in modules_list:
                m = QTreeWidgetItem(
                    ss_item
                  , [mod]
                  , cmake_help_parser.help_category.HELP_ITEM
                  )

        # Add policies group
        policies = QTreeWidgetItem(
            self.vewHelpPage.helpTargets
          , [i18nc('@item::inlistbox/plain', 'Policies')]
          , cmake_help_parser.help_category.POLICY
          )
        for pol in cmake_help_parser.get_cmake_policies_list():
            p = QTreeWidgetItem(
                policies
              , [pol]
              , cmake_help_parser.help_category.HELP_ITEM
              )

        # Add properties group
        properties = QTreeWidgetItem(
            self.vewHelpPage.helpTargets
          , [i18nc('@item::inlistbox/plain', 'Properties')]
          , cmake_help_parser.help_category.PROPERTY
          )
        for subsection, props_list in cmake_help_parser.get_cmake_properties_list().items():
            ss_item = QTreeWidgetItem(
                properties
              , [subsection]
              , cmake_help_parser.help_category.PROPERTY
              )
            for prop in props_list:
                v = QTreeWidgetItem(
                    ss_item
                  , [prop[0]]
                  , cmake_help_parser.help_category.HELP_ITEM
                  )
                v.setToolTip(0, prop[1])

        # Add variables group
        variables = QTreeWidgetItem(
            self.vewHelpPage.helpTargets
          , [i18nc('@item::inlistbox/plain', 'Variables')]
          , cmake_help_parser.help_category.VARIABLE
          )
        for subsection, vars_list in cmake_help_parser.get_cmake_vars_list().items():
            ss_item = QTreeWidgetItem(
                variables
              , [subsection]
              , cmake_help_parser.help_category.VARIABLE
              )
            for var in vars_list:
                v = QTreeWidgetItem(
                    ss_item
                  , [var[0]]
                  , cmake_help_parser.help_category.HELP_ITEM
                  )
                v.setToolTip(0, var[1])
        #
        self.vewHelpPage.helpTargets.resizeColumnToContents(0)


    @pyqtSlot()
    def updateHelpText(self):
        tgt = self.vewHelpPage.helpTargets.currentItem()
        if tgt is None or tgt.type() != cmake_help_parser.help_category.HELP_ITEM:
            return
        parent = tgt.parent()
        if parent is None:
            return

        category = parent.type()
        text = cmake_help_parser.get_help_on(category, tgt.text(0))

        if not self.cfgPage.htmlize.isChecked():
            self.helpPage.setText(text[text.index('\n') + 1:])
            return

        # TODO How *else* we can beautify the text?
        lines = text.splitlines()[1:]
        file_link_re = re.compile('Defined in: (.*)')
        file_link_sb = 'Defined in: <a href="file://\\1">\\1</a>'
        pre = False
        para = True
        for i, line in enumerate(lines):
            # Remove '&', '<' and '>' from text
            # TODO Use some HTML encoder instead of this...
            line = line.replace('&', '&amp;').replace('<', '&lt;').replace('>', '&gt;')
            #
            if line.lstrip().startswith('Defined in: '):
                line = file_link_re.sub(file_link_sb, line)
            #
            if i == 0:
                line = '<h1>{}</h1>'.format(line)
            elif line.startswith(' ' * cmake_help_parser.CMAKE_HELP_VARBATIM_TEXT_PADDING_SIZE):
                if not pre:
                    line = '<pre>' + line
                    pre = True
            elif len(line.strip()) == 0:
                if pre:
                    line = line + '</pre>'
                    pre = False
                elif para:
                    line = line + '</p>'
                    para = False
                else:
                    line = '<p>' + line
                    para = True
            lines[i] = line
        self.helpPage.setText('\n'.join(lines))


    @pyqtSlot(QTreeWidgetItem, int)
    def insertIntoCurrentDocument(self, item, column):
        if item is not None and column == 0:
            view = kate.activeView()
            document = kate.activeDocument()
            document.startEditing()
            document.insertText(view.cursorPosition(), item.text(0))
            document.endEditing()


    @pyqtSlot(QTreeWidgetItem, int)
    def insertHelpItemIntoCurrentDocument(self,item, column):
        if item is not None and item.type() == cmake_help_parser.help_category.HELP_ITEM and column == 0:
            view = kate.activeView()
            document = kate.activeDocument()
            document.startEditing()
            document.insertText(view.cursorPosition(), item.text(0))
            document.endEditing()


# ----------------------------------------------------------
# CMake utils: configuration stuff
# ----------------------------------------------------------

class CMakeConfigWidget(QWidget):
    '''Configuration widget for this plugin.'''

    cmakeBinary = None
    projectBuildDir = None

    def __init__(self, parent=None, name=None):
        super(CMakeConfigWidget, self).__init__(parent)

        # Set up the user interface from Designer.
        uic.loadUi(os.path.join(os.path.dirname(__file__), CMAKE_UTILS_SETTINGS_UI), self)

        self.reset();

        # Connect signals
        self.addButton.clicked.connect(self.addDir)
        self.removeButton.clicked.connect(self.removeDir)


    def apply(self):
        kate.configuration[CMAKE_BINARY] = self.cmakeBinary.text()
        kate.configuration[PARENT_DIRS_LOOKUP_CNT] = self.parentDirsLookupCnt.value()
        try:
            cmake_help_parser.validate_cmake_executable(kate.configuration[CMAKE_BINARY])
        except ValueError as error:
            ui.popup(
                i18nc('@title:window', 'Error')
              , i18nc('@info:tooltip', 'CMake executable test run failed:<nl/><message>%1</message>', error)
              , 'dialog-error'
              )
        # TODO Store the following for a current session!
        kate.configuration[PROJECT_DIR] = self.projectBuildDir.text()
        kate.configuration[AUX_MODULE_DIRS] = []
        for i in range(0, self.moduleDirs.count()):
            kate.configuration[AUX_MODULE_DIRS].append(self.moduleDirs.item(i).text())

        # Show some spam
        print('CMakeCC: config save: CMAKE_BINARY={}'.format(kate.configuration[CMAKE_BINARY]))
        print('CMakeCC: config save: AUX_MODULE_DIRS={}'.format(kate.configuration[AUX_MODULE_DIRS]))
        print('CMakeCC: config save: PROJECT_DIR={}'.format(kate.configuration[PROJECT_DIR]))
        kate.configuration.save()


    def reset(self):
        self.defaults()
        if CMAKE_BINARY in kate.configuration:
            self.cmakeBinary.setText(kate.configuration[CMAKE_BINARY])
        if PARENT_DIRS_LOOKUP_CNT in kate.configuration:
            self.parentDirsLookupCnt.setValue(kate.configuration[PARENT_DIRS_LOOKUP_CNT])
        if AUX_MODULE_DIRS in kate.configuration:
            self.moduleDirs.addItems(kate.configuration[AUX_MODULE_DIRS])
        if PROJECT_DIR in kate.configuration:
            self.projectBuildDir.setText(kate.configuration[PROJECT_DIR])


    def defaults(self):
        # TODO Dectect it!
        self.cmakeBinary.setText(CMAKE_BINARY_DEFAULT)
        self.parentDirsLookupCnt.setValue(0)


    @pyqtSlot()
    def addDir(self):
        path = KFileDialog.getExistingDirectory(
            KUrl('')
          , self
          , i18nc('@title:window', 'Select a Directory with CMake Modules')
          )
        print('CMakeCC: got path={}'.format(path))
        self.moduleDirs.addItem(str(path))


    @pyqtSlot()
    def removeDir(self):
        # Damn UGLY! Smth wrong w/ Qt definitely...
        item = self.moduleDirs.takeItem(self.moduleDirs.currentRow())
        item = None



class CMakeConfigPage(kate.Kate.PluginConfigPage, QWidget):
    '''Kate configuration page for this plugin.'''
    def __init__(self, parent=None, name=None):
        super(CMakeConfigPage, self).__init__(parent, name)
        self.widget = CMakeConfigWidget(parent)
        lo = parent.layout()
        lo.addWidget(self.widget)

    def apply(self):
        self.widget.apply()

    def reset(self):
        self.widget.reset()

    def defaults(self):
        self.widget.defaults()
        self.changed.emit()


@kate.configPage(
    i18nc('@action:inmenu', 'CMake Helper Plugin')
  , i18nc('@title:group', 'CMake Helper Settings')
  , icon='cmake'
  )
def cmakeConfigPage(parent=None, name=None):
    return CMakeConfigPage(parent, name)

@kate.viewCreated
def createSignalAutocompleteCMake(view=None, *args, **kwargs):
    print('CMakeCC: Register completion model')
    try:
        view = view or kate.activeView()
        if view:
            cci = view.codeCompletionInterface()
            cci.registerCompletionModel(cmake_completion_model)
    except:
        print('CMake Helper Plugin: Unable to get an active view')


@kate.init
def init():
    # Set default value if not configured yet
    print('CMakeCC: enter init')
    if CMAKE_BINARY not in kate.configuration:
        kate.configuration[CMAKE_BINARY] = CMAKE_BINARY_DEFAULT
    if PARENT_DIRS_LOOKUP_CNT not in kate.configuration:
        kate.configuration[PARENT_DIRS_LOOKUP_CNT] = 0
    if AUX_MODULE_DIRS not in kate.configuration:
        kate.configuration[AUX_MODULE_DIRS] = []
    if PROJECT_DIR not in kate.configuration:
        kate.configuration[PROJECT_DIR] = ''
    if TOOLVIEW_ADVANCED_MODE not in kate.configuration:
        kate.configuration[TOOLVIEW_ADVANCED_MODE] = False
    if TOOLVIEW_BEAUTIFY not in kate.configuration:
        kate.configuration[TOOLVIEW_BEAUTIFY] = True

    # Initialize completion model
    createSignalAutocompleteCMake()

    # Make an instance of a cmake tool view
    global cmakeToolView
    if cmakeToolView is None:
        cmakeToolView = CMakeToolView(kate.mainWindow())


@kate.unload
def destroy():
    '''Plugins that use a toolview need to delete it for reloading to work.'''
    global cmakeToolView
    if cmakeToolView:
        cmakeToolView.__del__()
        cmakeToolView = None

cmake_completion_model = CMakeCompletionModel(kate.application)
cmake_completion_model.modelReset.connect(_reset)


# kate: indent-width 4;