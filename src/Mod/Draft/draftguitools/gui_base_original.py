# ***************************************************************************
# *   (c) 2009 Yorik van Havre <yorik@uncreated.net>                        *
# *   (c) 2010 Ken Cline <cline@frii.com>                                   *
# *   (c) 2020 Eliud Cabrera Castillo <e.cabrera-castillo@tum.de>           *
# *                                                                         *
# *   This file is part of the FreeCAD CAx development system.              *
# *                                                                         *
# *   This program is free software; you can redistribute it and/or modify  *
# *   it under the terms of the GNU Lesser General Public License (LGPL)    *
# *   as published by the Free Software Foundation; either version 2 of     *
# *   the License, or (at your option) any later version.                   *
# *   for detail see the LICENCE text file.                                 *
# *                                                                         *
# *   FreeCAD is distributed in the hope that it will be useful,            *
# *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
# *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
# *   GNU Library General Public License for more details.                  *
# *                                                                         *
# *   You should have received a copy of the GNU Library General Public     *
# *   License along with FreeCAD; if not, write to the Free Software        *
# *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  *
# *   USA                                                                   *
# *                                                                         *
# ***************************************************************************
"""Provides the base classes for most old Draft Gui Commands.

This class is used by Gui Commands to set up some properties
of the DraftToolBar, the Snapper, and the working plane.
"""
## @package gui_base_original
# \ingroup draftguitools
# \brief Provides the base classes for most old Draft Gui Commands.

## \addtogroup draftguitools
# @{

from PySide import QtCore

import FreeCAD as App
import FreeCADGui as Gui
import DraftVecUtils
import WorkingPlane
from draftguitools import gui_tool_utils
from draftguitools import gui_trackers as trackers
from draftutils import gui_utils
from draftutils import params
from draftutils import todo
from draftutils import utils
from draftutils.messages import _log, _toolmsg


class DraftTool:
    """The base class of all Draft Tools.

    This is the original class that was defined in `DraftTools.py`
    before any re-organization of the code.

    This class is subclassed by `Creator` and `Modifier`
    to set up a few additional properties of these two types.

    This class connects with the `draftToolBar` and `Snapper` classes
    that are installed in the `FreeCADGui` namespace in order to set up some
    properties of the running tools such as the task panel, the snapping
    functions, and the grid trackers.

    It also connects with the `WorkingPlane` class.

    This class is intended to be replaced by newer classes inside the
    `gui_base` module, in particular, `GuiCommandBase`.
    """

    def __init__(self):
        self.commitList = []

    def IsActive(self):
        """Return True when this command should be available."""
        return bool(gui_utils.get_3d_view())

    def Activated(self, name="None", is_subtool=False):
        """Execute when the command is called.

        If an active Gui Command exists, it will call the `finish` method
        of it to terminate it.

        If no active Gui Command exists, it will proceed with configuration
        of the tool. The child class that subclasses this class
        then should continue with its own Activated method.

        Parameters
        ----------
        name: str, optional
            It defaults to `'None'`.
            It is the `featureName` of the object, to know what is being run.

        is_subtool: bool, optional
            It defaults to `False`.
            This is set to `True` when we want to modify an object
            by using the mechanism of a `subtool`, introduced
            through the `Draft_SubelementHighlight` command.
            That is, first we run `Draft_SubelementHighlight`
            then we can use `Draft_Move` while setting `is_subtool` to `True`.
        """
        if App.activeDraftCommand and not is_subtool:
            App.activeDraftCommand.finish()
        App.activeDraftCommand = self

        # The Part module is first initialized when using any Gui Command
        # for the first time.
        global Part, DraftGeomUtils
        import Part
        import DraftGeomUtils

        self.call = None
        self.commitList = []
        self.constrain = None
        self.doc = App.ActiveDocument
        self.extendedCopy = False
        self.featureName = name
        self.node = []
        self.obj = None
        self.point = None
        self.pos = []
        self.support = None
        self.ui = Gui.draftToolBar
        self.ui.mouse = True  # reset mouse movement
        self.ui.sourceCmd = self
        self.view = gui_utils.get_3d_view()
        self.wp = WorkingPlane.get_working_plane()

        self.planetrack = None
        if params.get_param("showPlaneTracker"):
            self.planetrack = trackers.PlaneTracker()
        if hasattr(Gui, "Snapper"):
            Gui.Snapper.setTrackers()

        _toolmsg("{}".format(16*"-"))
        _toolmsg("GuiCommand: {}".format(self.featureName))

        # update hints after the tool is fully initialized
        QtCore.QTimer.singleShot(0, self.updateHints)

    def updateHints(self):
        Gui.HintManager.show(*self.getHints())

    def getHints(self):
        return [
            Gui.InputHint("%1 constrain", Gui.UserInput.KeyShift)
        ]

    def end_callbacks(self, call):
        try:
            self.view.removeEventCallback("SoEvent", call)
            gui_utils.end_all_events()
        except RuntimeError:
            # the view has been deleted already
            pass
        call = None

    def finish(self, cont=False):
        """Finish the current command.

        These are general cleaning tasks that are performed
        when terminating all commands.

        These include setting the node list to empty,
        setting to `None` the active command,
        turning off the graphical interface (task panel),
        finishing the plane tracker, restoring the working plane,
        turning off the snapper.

        If a callback is installed in the 3D view, the callback is removed,
        and set to `None`.

        If the commit list is non-empty it will commit the instructions on
        the list with `draftutils.todo.ToDo.delayCommit`,
        and the list will be set to empty.
        """
        self.node = []
        App.activeDraftCommand = None
        if self.ui:
            self.ui.offUi()
            self.ui.sourceCmd = None
        if hasattr(Gui, "Snapper"):
            Gui.Snapper.off()
        if self.planetrack:
            self.planetrack.finalize()
        self.wp._restore()
        if self.commitList:
            last_cmd = self.commitList[-1][1][-1]
            if last_cmd.find("recompute") >= 0:
                self.commitList[-1] = (self.commitList[-1][0], self.commitList[-1][1][:-1])
                todo.ToDo.delayCommit(self.commitList)
                todo.ToDo.delayAfter(Gui.doCommand, last_cmd)
            else:
                todo.ToDo.delayCommit(self.commitList)
        self.commitList = []

        Gui.HintManager.hide()

    def commit(self, name, func):
        """Store actions in the commit list to be run later.

        Parameters
        ----------
        name: str
            An arbitrary string that indicates the name of the operation
            to run.

        func: list of str
            Each element of the list is a string that will be run by
            `Gui.doCommand`.

            See the complete information in the `draftutils.todo.ToDo` class.
        """
        self.commitList.append((name, func))

    def getStrings(self, addrot=None):
        """Return useful strings that will be used to build commands.

        Returns
        -------
        str, str, str, str
            A tuple of four strings that represent useful information.

            * the current working plane rotation quaternion as a string
            * the support object if available as a string
            * the list of nodes inside the `node` attribute as a string
            * the string `'True'` or `'False'` depending on the make face
              setting
        """
        # Current plane rotation as a string
        qr = self.wp.get_placement().Rotation.Q
        qr = "({0}, {1}, {2}, {3})".format(qr[0], qr[1], qr[2], qr[3])

        # Support object
        if self.support and params.get_param("useSupport"):
            sup = 'FreeCAD.ActiveDocument.getObject'
            sup += '("{}")'.format(self.support.Name)
        else:
            sup = 'None'

        # Contents of self.node
        points = '['
        for n in self.node:
            if len(points) > 1:
                points += ', '
            points += DraftVecUtils.toString(n)
        points += ']'

        # Make face
        if self.ui:
            make_face = str(self.ui.makeFaceMode)
        else:
            make_face = "True"

        return qr, sup, points, make_face


class Creator(DraftTool):
    """A generic Creator tool, used by creation tools such as line or arc.

    It inherits `DraftTool`, which sets up the majority of the behavior
    of this class.
    """

    def Activated(self, name="None"):
        """Execute when the command is called.

        Parameters
        ----------
        name: str, optional
            It defaults to `'None'`.
            It is the `featureName` of the object, to know what is being run.
        """
        super().Activated(name)
        # call _save to sync with _restore called in finish method
        self.wp._save()
        self.support = gui_tool_utils.get_support()


class Modifier(DraftTool):
    """A generic Modifier tool, used by modification tools such as move.

    After initializing the parent class, it sets the `copymode` attribute
    to `False`.

    It inherits `DraftTool`, which sets up the majority of the behavior
    of this class.
    """

    def __init__(self):
        super().__init__()
        self.copymode = False

    def Activated(self, name="None", is_subtool=False):
        super().Activated(name, is_subtool)
        # call _save to sync with _restore called in finish method
        self.wp._save()
## @}
