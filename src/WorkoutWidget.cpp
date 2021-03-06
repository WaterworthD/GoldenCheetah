/*
 * Copyright (c) 2015 Mark Liversedge (liversedge@gmail.com)
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc., 51
 * Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */


#include "WorkoutWidget.h"
#include "WorkoutWindow.h"
#include "WorkoutWidgetItems.h"

#include "WPrime.h"
#include "ErgFile.h"
#include "RideFile.h"
#include "RideFileCache.h"
#include "RealtimeData.h"

#include "TimeUtils.h" // time_to_string()
#include <QFontMetrics>

#include <cmath>
#include <float.h> // DBL_EPSILON

static int MINTOOLHEIGHT = 350; // minimum size for a full editor

void WorkoutWidget::adjustLayout()
{
    // adjust all the settings based upon current size
    if (height() > MINTOOLHEIGHT) {

        // big, can edit and all widgets shown
        IHEIGHT = 10;
        THEIGHT = 35;
        BHEIGHT = 35;
        LWIDTH = 65;
        RWIDTH = 35;
        XTICLENGTH = 3;
        YTICLENGTH = 0;
        XTICS = 20;
        YTICS = 10;
        SPACING = 2; // between labels and tics (if there are tics)
        XMOVE = 5; // how many to move X when cursoring
        YMOVE = 1; // how many to move Y when cursoring
        GRIDLINES = true;

    } else {

        // mini mode
        IHEIGHT = 0;
        THEIGHT = 0;
        BHEIGHT = 20;
        LWIDTH = 10;
        RWIDTH = 10;
        XTICLENGTH = 3;
        YTICLENGTH = 0;
        XTICS = 20;
        YTICS = 5;
        SPACING = 2; // between labels and tics (if there are tics)
        XMOVE = 5; // how many to move X when cursoring
        YMOVE = 1; // how many to move Y when cursoring
        GRIDLINES = false;
    }
}

WorkoutWidget::WorkoutWidget(WorkoutWindow *parent, Context *context) :
    QWidget(parent),  state(none), ergFile(NULL), dragging(NULL), parent(parent), context(context), stackptr(0), recording_(false)
{
    maxX_=3600;
    maxY_=400;

    // when plotting telemetry these are maxY for those series
    cadenceMax = 200; // make it line up between power and hr
    hrMax = 220;
    speedMax = 50;

    onDrag = onCreate = onRect = atRect = QPointF(-1,-1);

    // watch mouse events for user interaction
    adjustLayout();
    installEventFilter(this);
    setMouseTracking(true);

    connect(context, SIGNAL(configChanged(qint32)), this, SLOT(configChanged(qint32)));
    connect(context, SIGNAL(ergFileSelected(ErgFile*)), this, SLOT(ergFileSelected(ErgFile*)));
    connect(context, SIGNAL(telemetryUpdate(RealtimeData)), this, SLOT(telemetryUpdate(RealtimeData)));
    configChanged(CONFIG_APPEARANCE);
}

void
WorkoutWidget::start()
{
    recording_ = true;

    // if we have edited the erg we need to update the in-memory points
    if (ergFile && stack.count()) {

        // replace all the points
        ergFile->Points.clear();
        foreach(WWPoint *p, points_) {
            ergFile->Points.append(ErgFilePoint(p->x * 1000, p->y, p->y));
        }

        // force any other plots to take the changes
        context->notifyErgFileSelected(ergFile);
    }

    // clear previous data
    wbal.clear();
    watts.clear();
    hr.clear();
    speed.clear();
    cadence.clear();

    // and resampling data
    count = wbalSum = wattsSum = hrSum = speedSum = cadenceSum =0;

    // set initial 
    cadenceMax = 200;
    hrMax = 220;
    speedMax = 50;

    // replot
    update();
}

void
WorkoutWidget::stop()
{
    recording_ = false;
    update();
}

void 
WorkoutWidget::telemetryUpdate(RealtimeData rt)
{
    // only plot when recording
    if (!recording_) return;

    wbalSum += rt.getWbal();
    wattsSum += rt.getWatts();
    hrSum += rt.getHr();
    cadenceSum += rt.getCadence();
    speedSum += rt.getSpeed();

    count++;

    // did we get 5 samples (5hz refresh rate) ?
    if (count == 5) {
        int b = wbalSum / 5.0f;
        wbal << b;
        int w = wattsSum / 5.0f;
        watts << w;
        int h = hrSum / 5.0f;
        hr << h;
        double s = speedSum / 5.0f;
        speed << s;
        int c = cadenceSum / 5.0f;
        cadence << c;

        // clear for next time
        count = wbalSum = wattsSum = hrSum = speedSum = cadenceSum =0;

        // do we need to increase maxes?
        if (c > cadenceMax) cadenceMax=c;
        if (s > speedMax) speedMax=s;
        if (h > hrMax) hrMax=h;

        // replot
        update();
    }
}

void
WorkoutWidget::timeout()
{
    // into event filter (our state machine)
    QEvent timer(QEvent::Timer);
    eventFilter(this, &timer);
}


// Inbound events are processed through a "state machine" that reacts
// to each event depending upon the current state.
//
// Since there are only a handful of states and the transitions are
// not complex this is performed using basic if/else clauses rather
// than an FSM.
//
// The state transitions are complex enough to need documenting:
//
//
// STATE MACHINE
//
// # EVENT              STATE       ACTION                              NEXT STATE
// - --------------     ------      ---------------                     ----------
// 1 mouse move         none        hover/unhover point/block           none
//                      none        hover/unhover lap marker            none
//                      drag        move point around                   drag
//                      dragblock   move block around                   dragblock
//                      rect        resize and scan for selections      rect
//                      create      add point and move it               drag
//
// 2 mouse click        none        hovering? drag point                drag
//   not shifted        none        not hovering? set to create         create
//
// 3 mouse release      drag        unselect                            none
//                      rect        none                                none
//                      create      create point                        none
//                      dragblock   create block                        none
//
// 4 mouse timeout      create      create block                        dragblock
//                      drag        ignore                              drag
//
//
// 5 mouse wheel        none        rescale selectes/all                none
//   up and down        drag        ignore                              drag
//
//
// 6 mouse click        none        hovering? select point              none
//   shifted            none        not hovering? begin select          rect
//
//
// 7 key press          any         ESC clear selection                 unchanged
//                      any         ^Z undo, ^Y redo ^X cut             unchanged
//                      any         ^A select all
//                      any         cursors move selected               unchanged
//                      any         DEL delete selected                 unchanged
//
//
// 8 mouse enter        any         grab keyboard focus                 unchanged
//
// 9 screen resize      any         recalculate geometry objects        unchanged
//                                  e.g. selection/cursor Block
//

bool
WorkoutWidget::eventFilter(QObject *obj, QEvent *event)
{
    // process as normal if not one of ours
    if (obj != this) return false;

    // where is the cursor
    QPoint p = mapFromGlobal(QCursor::pos());
    QPointF v = reverseTransform(p.x(),p.y());

    // is a repaint going to be needed?
    bool updateNeeded=false;

    // are we filtering out the event? (e.g. keyboard / scroll)
    bool filterNeeded=false;

    //
    // 1 MOUSE MOVE [we always repaint]
    //
    if (event->type() == QEvent::MouseMove) {

        // forget the onCreate!
        onCreate = QPoint(-1,-1);

        // always update the x/y values in the toolbar
        parent->xlabel->setText(time_to_string(v.x()));
        parent->ylabel->setText(QString("%1w").arg(v.y()));

        // STATE: NONE
        if (state == none) {

            // if we're not in any particular state then just
            // highlight for hover/unhover
            updateNeeded = setBlockCursor();

            // is the mouse on the canvas?
            if (canvas().contains(p)) {

                // unser/set hover/unhover state *IF NEED TO*
                foreach(WWPoint *point, points_) {
                    if (point->bounding().contains(p)) {
                        if (!point->hover) {
                            point->hover = true;
                            updateNeeded=true;
                        }
                    } else {
                        if (point->hover) {
                            point->hover = false;
                            updateNeeded=true;
                        }
                    }
                }
            }

            // set lap marker state if needed, but don't
            // lost the updateNeeded state if already true
            updateNeeded= updateNeeded || setLapState();

        // STATE: CREATE
        } else if (state == create) {

            // moved before timeout on create
            updateNeeded = createPoint(p);

            // now get ready to drag
            state = drag;

            // recompute metrics
            recompute();

        // STATE: DRAG
        } else if (state == drag) {

            // we're dragging this point around, get on and
            // do that, but apply constrains
            if (dragging) {
                updateNeeded = movePoint(p);

                // this may possibly be too expensive
                // on slower hardware?
                recompute();

            } else {
                // not possible?
                state = none;
                qDebug()<<"WW FSM: drag state dragging=NULL";
            }

        // STATE: RECT
        } else if (state == dragblock) {

            // move it
            updateNeeded = moveBlock(p);

            // we moved the block
            recompute();

        } else if (state == rect) {

            // we're selecting via a rectangle
            atRect = p;
            updateNeeded = selectPoints(); // go and check / select
        }
    }

    //
    // 2 AND 6 MOUSE PRESS
    //
    if (event->type() == QEvent::MouseButtonPress) {

        // watch for shift when clicking
        Qt::KeyboardModifiers kmod = static_cast<QInputEvent*>(event)->modifiers();

        // if not in draw mode toggle shift to select
        // so if press shift in draw -> select
        // if press shift in select -> draw
        if (parent->draw == false) kmod ^= Qt::ShiftModifier;

        // STATE: NONE
        if (state == none && canvas().contains(p)) {

            // either select existing to drag
            // or create a new one to drag
            bool hover=false;
            foreach(WWPoint *point, points_) {
                if (point->bounding().contains(p)) {

                    //
                    // HOVERING
                    //

                    // SHIFT-CLICK TO TOGGLE SELECT
                    if (kmod & Qt::ShiftModifier) {
                        point->selected = !point->selected;
                        hover=true;
                        updateNeeded=true;
                    } else {

                        // PLAIN CLICK TO DRAG
                        updateNeeded=true;
                        dragging = point;
                        hover=true;
                        onDrag = QPointF(dragging->x, dragging->y);
                        state = drag;

                    }
                    break;
                }
            }

            // if state is still none and we're not hovering,
            // we aren't on top of a point, so create a new
            // one or start select mode if shift is pressed
            if (state == none && !hover) {

                // SHIFT RECTANGLE SELECT
                if (kmod & Qt::ShiftModifier) {

                    // where are we
                    atRect = onRect = p;
                    state = rect;
                    updateNeeded = true;

                } else {

                    // UNSHIFTED CREATE A POINT
                    state = create;

                    // but we may press and hold for a snip
                    // so lets set the timer and remember
                    // where we were
                    onCreate = p;
                    QTimer::singleShot(500, this, SLOT(timeout()));
                }
            }
        }
    }

    //
    // 3. MOUSE RELEASED
    //
    if (event->type() == QEvent::MouseButtonRelease) {

        // STATE: DRAG
        if (state == drag && dragging) {

            // create command to reflect the drag, but only
            // if it actually moved!
            if (dragging->x != onDrag.x() || dragging->y != onDrag.y())
                new MovePointCommand(this, onDrag, QPointF(dragging->x, dragging->y), points_.indexOf(dragging));

            // recompute metrics
            recompute();
        }

        // STATE: DRAG BLOCK
        if (state == dragblock && cr8block.count()) {
            new CreateBlockCommand(this, cr8block);

            // now recompute
            recompute();
        }

        // STATE: RECT
        if (state == rect) {
            selectedPoints();
            onRect = atRect = QPointF(-1,-1);

        // STATE: CREATE
        } else if (state == create) {

            // moved before timeout on create
            updateNeeded = createPoint(p);

            // recompute metrics
            recompute();
        }

        foreach(WWPoint *point, points_) point->hover=false;
        state = none;
        dragging = NULL;
        updateNeeded = true;
    }

    //
    // 4. MOUSE TIMEOUT [click and hold]
    //
    if (event->type() == QEvent::Timer) {

        // STATE: CREATE
        if (state == create && onCreate == p) {

            // if we are still on state create from initial click
            // then we can create, otherwise just ignore

            // create a block
            updateNeeded = createBlock(p);

            // recompute metrics
            recompute();

            // set state to dragblock
            state = dragblock;
        }

    }

    //
    // 5. MOUSE WHEEL
    //
    if (event->type() == QEvent::Wheel) {

        // STATE: NONE
        if (state == none) {
            QWheelEvent *w = static_cast<QWheelEvent*>(event);
#if QT_VERSION >= 0x050000
            updateNeeded = scale(w->angleDelta());
#else
            updateNeeded = scale(QPoint(0,w->delta()));
#endif
            filterNeeded = true;
        }

        // will need to ..
        recompute();
    }

    //
    // 7. KEYPRESS
    //
    if (event->type() == QEvent::KeyPress) {

        // STATE: ANY (!)

        // we care about cmd / ctrl
        Qt::KeyboardModifiers kmod = static_cast<QInputEvent*>(event)->modifiers();
        bool ctrl = (kmod & Qt::ControlModifier) != 0;
        int key;

        switch((key=static_cast<QKeyEvent*>(event)->key())) {

        case Qt::Key_Up:
        case Qt::Key_Down:
        case Qt::Key_Left:
        case Qt::Key_Right:
            filterNeeded = true; // we grab all key events
            updateNeeded=movePoints(key,kmod);
            break;

        case Qt::Key_Escape:
            filterNeeded = true; // we grab all key events
            updateNeeded=selectClear();
            break;

        case Qt::Key_C:
            if (ctrl) {
                filterNeeded = true; // we grab all key events
                copy();
            }
            break;

        case Qt::Key_V:
            if (ctrl) {
                filterNeeded = true; // we grab all key events
                paste();
            }
            break;

        case Qt::Key_X:
            if (ctrl) {
                filterNeeded = true; // we grab all key events
                cut();
            }
            break;

        case Qt::Key_A:
            if (ctrl) {
                filterNeeded = true; // we grab all key events
                updateNeeded=selectAll();
            }
            break;

        case Qt::Key_Y:
            if (ctrl) {
                redo();
                filterNeeded = true; // we grab all key events
                updateNeeded=true;
            }
            break;

        case Qt::Key_Z:
            if (ctrl) {
                undo();
                filterNeeded = true; // we grab all key events
                updateNeeded=true;
            }
            break;

        case Qt::Key_Delete:
            // delete!
            filterNeeded = true; // we grab all key events
            updateNeeded=deleteSelected();
            break;
        }

        // we moved / deleted etc, so redo metrics et al
        if (updateNeeded) recompute();
    }

    //
    // 8. MOUSE ENTERS
    //
    if (event->type() == QEvent::Enter) {

        // STATE: ANY
        if (!hasFocus()) {
            setFocus(Qt::MouseFocusReason);
        }
    }

    //
    // 9. RESIZE EVENT
    //
    if (event->type() == QEvent::Resize) {

        // we need to adjust layout and repaint
        adjustLayout(); 
        updateNeeded = true;
    }


    // ALL DONE

    // trigger an update if one is needed
    if (updateNeeded) {

        // the cursor may now hover over a block or point
        // but don't interfere whilst state processing
        if (state == none) setBlockCursor();

        // repaint
        update();
    }

    // return false - we are eavesdropping not processing.
    // except for wheel events which we steal
    return filterNeeded;
}

bool
WorkoutWidget::setLapState()
{
    // by default nothing to do
    bool returning = false;

    // have laps been hovered/unhovered?
    if (laps_.count()==0) return false;

    // where is the cursor
    QPoint p = mapFromGlobal(QCursor::pos());
    int x = reverseTransform(p.x(),0).x();

    bool intop = top().contains(p);

    // run through lap markers..
    for(int i=0; i<laps_.count(); i++) {

        if (!intop) {

            // unselect regardless as cursor not there, notify if needed
            if (laps_[i].selected == true) returning = true;
            laps_[i].selected = false;

        } else {
            // we need to work out if the cursor is for us
            if ((x > (laps_[i].x/1000.00f)) && (i == (laps_.count()-1) || x < (laps_[i+1].x/1000.00f))) { // cursor to right

                // select and notify it changed
                if (!laps_[i].selected) returning=true;
                laps_[i].selected = true;

            } else {

                // nope, so deselect and notify if changed
                if (laps_[i].selected) returning=true;
                laps_[i].selected = false;
            }
        }
    }

    return returning;
}

bool
WorkoutWidget::setBlockCursor()
{
    // where is the mouse?
    QPoint c = mapFromGlobal(QCursor::pos());

    //
    // SELECTION BLOCK - block created by selecting points
    //

    // lets set the selection block first, coz if the cursor
    // first and last index of selected items
    int begin=-1, end=-1;
    for(int i=0; i<points_.count(); i++) {
        if (points_[i]->selected) {
            if (begin == -1) begin = i;
            end = i;
        }
    }

    // if we need a path, lets create one
    if (begin >=0 && end >= 0 && points_[begin]->x < points_[end]->x) {

        // accumalate joules and time
        double joules=0;
        double secs=0;

        // create a painterpath for all the selected blocks
        QPointF firstp = transform(points_[begin]->x, 0);
        QPainterPath block(firstp); // origin
        for (int i=begin; i <= end; i++) {

            // accumalate
            if (i != begin) {
                double duration = points_[i]->x - points_[i-1]->x;
                joules += (points_[i]->y + points_[i-1]->y) / 2 * duration;
                secs += duration;
            }

            QPointF here = transform(points_[i]->x, points_[i]->y);
            block.lineTo(here);
        }

        // and back again
        QPointF lastp = transform(points_[end]->x, 0);
        block.lineTo(lastp);
        block.lineTo(firstp);

        // done
        selectionBlock = block;

        // average power
        selectionBlockText2 = QString("%1w").arg(joules/secs, 0, 'f', 0);
        selectionBlockText = time_to_string(secs);

        parent->copyAct->setEnabled(true);
        parent->cutAct->setEnabled(true);

    } else {

        selectionBlock = QPainterPath();
        selectionBlockText = selectionBlockText2 = "";

        parent->copyAct->setEnabled(false);
        parent->cutAct->setEnabled(false);
    }

    //
    // CURSOR BLOCK -- HOVER BLOCK AS WE MOVE MOUSE
    //

    // not on canvas?
    if (!canvas().contains(c)) {
        if (cursorBlock != QPainterPath()) {
            cursorBlock = QPainterPath();
            return true;
        }
        return false;
    }

    bool returning=false;
    QPointF last(0,0);
    int lastx=0;
    int lasty=0;

    foreach(WWPoint *p, points_) {

        // might be better to always use float?
        QPoint center = transform(p->x,p->y);
        QPointF dot(center.x(), center.y());

        // cursor in ?
        if (last.x() > 0 && c.x() > last.x() && c.x() < dot.x() && dot.x() > last.x()) {

            // found the cursor, but is it in the block?
            QPointF begin(last.x(), canvas().bottom());
            QPainterPath block(begin);
            block.lineTo(last);
            block.lineTo(dot);
            block.lineTo(dot.x(),begin.y());
            block.lineTo(begin);

            // is it inside?
            if (block.contains(c)) {

                // if different then update and want a repaint
                if (cursorBlock != block) {
                    cursorBlock = block;
                    cursorBlockText = time_to_string(p->x - lastx);
                    cursorBlockText2= QString("%1w").arg(double(lasty + ((p->y-lasty)/2)), 0, 'f', 0);
                    returning = true;
                }
            } else if (cursorBlock != QPainterPath()) {

                // not inside but not set to null
                cursorBlock = QPainterPath();
                returning = true;
            }
            break;
        }

        // moving on
        last = dot;
        lastx = p->x;
        lasty = p->y;
    }
    return returning;
}

static bool doubles_equal(double a, double b)
{
    double errorB = b * DBL_EPSILON;
    return (a >= b - errorB) && (a <= b + errorB);
}

// move the selected points in he direction of the cursor key
// constrained to the limits of the points not selected
bool
WorkoutWidget::movePoints(int key, Qt::KeyboardModifiers kmod)
{
    Q_UNUSED(kmod); // perhaps in the future...

    // apply constraints, don't move if we are constrained
    // by moving off the scale or create a workout that is
    // invalid (e.g. time goes backwards
    bool constrained = false;

    for(int index=0; index < points_.count(); index++) {

        WWPoint *p = points_[index];

        // going left
        if (key == Qt::Key_Left) {

            // look at prev
            WWPoint *prev = index ? points_[index-1] : NULL;

            // hit the start of the workout
            if (p->selected && (p->x-XMOVE) < 0) { constrained=true; break; }

            // hit the previous unselected
            if (p->selected && prev && !prev->selected && prev->x > (p->x-XMOVE)) {
                constrained=true; break;
            }
        }

        // going right
        if (key == Qt::Key_Right) {

            // look at next
            WWPoint *next = index+1 < points_.count() ? points_[index+1] : NULL;

            // hit the end of the workout
            if (p->selected && (p->x+XMOVE) > maxX()) { constrained=true; break; }

            // hit the next unselected
            if (p->selected && next && !next->selected && next->x < (p->x+XMOVE)) {
                constrained=true; break;
            }
        }

        // going down
        if (key == Qt::Key_Down) {

            // hit zero
            if (p->selected && (p->y-YMOVE) < 0) { constrained=true; break; }
        }
    }

    // no dice, skip update we changed nothing
    if (constrained) return false;

    // create command as we go
    QList<PointMemento> before, after;

    // visit every point and move if it is selected
    for(int index=0; index < points_.count(); index++) {

        WWPoint *p = points_[index];

        // just selected ones
        if (p->selected == false) continue;

        // add before
        before << PointMemento(p->x, p->y, index);

        switch(key) {

        case Qt::Key_Up:
            p->y += YMOVE;
            break;

        case Qt::Key_Down:
            p->y -= YMOVE;
            break;

        case Qt::Key_Left:
            p->x -= XMOVE;
            break;

        case Qt::Key_Right:
            p->x += XMOVE;
            break;
        }

        // add after
        after << PointMemento(p->x, p->y, index);
    }

    if (before.count()) {
        new MovePointsCommand(this, before, after);
        return true;
    }

    return false;
}

bool
WorkoutWidget::movePoint(QPoint p)
{
    int index = points_.indexOf(dragging); // XXX optimise this out on set drag state
    QPoint f = transform(dragging->x, dragging->y); // current loc of point
    QPointF to = reverseTransform(p.x(), p.y()); // watts/secs to move to

    // boom .. we don't exist!
    if (index == -1) return false;

    // moving left
    if (p.x() < f.x()) {

        // we are constrained by another point?
        if (index > 0) {

            // get constraining point to the left
            QPoint c = transform(points_[index-1]->x, points_[index-1]->y); // current loc of point

            // not beyond just move, otherwise align to constraint
            if (c.x() < p.x()) dragging->x = to.x();
            else dragging->x = points_[index-1]->x;

        } else {

            // unconstrained
            dragging->x = to.x();

        }
    }

    // moving right
    if (p.x() > f.x()) {

        // we are constrained by another point?
        if ((index+1) < points_.count()) {

            // get constraining point to the right
            QPoint c = transform(points_[index+1]->x, points_[index+1]->y); // current loc of point

            // not beyond just move, otherwise align to constraint
            if (c.x() > p.x()) dragging->x = to.x();
            else dragging->x = points_[index+1]->x;

        } else {

            // unconstrained
            dragging->x = to.x();

        }
    }

    // we don't constrain y, but highlight points that align
    foreach(WWPoint *point, points_) {
        if (point == dragging) continue;

        point->hover=false;
        if (doubles_equal(point->y, to.y())) point->hover = true;
    }
    dragging->y = to.y();
    return true;
}

bool
WorkoutWidget::createPoint(QPoint p)
{
    // add a point!
    QPointF to = reverseTransform(p.x(), p.y());

    // don't auto append, we are going to insert     vvvvv
    WWPoint *add = new WWPoint(this, to.x(), to.y(), false);

    onDrag = QPointF(add->x, add->y);      // yuk, this should be done in the FSM (eventFilter)
                                           // action at a distance ... yuk XXX TIDY THIS XXX
    // add into the points
    for(int i=0; i<points_.count(); i++) {
        if (points_[i]->x > to.x()) {
            points_.insert(i, add);
            new CreatePointCommand(this, to.x(), to.y(), i);

            // enter drag mode -- add command resets we
            // are an edge case so handle it ourselves
            state = drag;
            dragging = add;
            return true;
        }
    }

    // after current
    points_.append(add);
    new CreatePointCommand(this, to.x(), to.y(), -1);

    // enter drag mode - edge case as above
    state = drag;
    dragging = add;
    return true;
}

bool
WorkoutWidget::moveBlock(QPoint p)
{
    // we are drag creating blocks
    // Remove any that might be there
    // delete the points in reverse
    for (int i=cr8block.count()-1; i>=0; i--) {
        WWPoint *p = NULL;
        if (cr8block[i].index >= 0) p = points_.takeAt(cr8block[i].index);
        else p = points_.takeAt(points_.count()-1);
        delete p;
    }

    // stop these cr8block
    cr8block.clear();

    // now create again
    createBlock(p);

    // refresh
    return true;
}

bool
WorkoutWidget::createBlock(QPoint p)
{
    // just in case
    cr8block.clear();

    // if between points we INSERT, if at the end
    // we APPEND
    WWPoint *add;

    // add a point!
    QPointF to = reverseTransform(p.x(), p.y());
    QList<WWPoint *> adding;

    // nothing there yet, create first flat block
    if (points_.count() == 0) {

        //
        // Empty workout so create a single block starting from x=0
        //
        add = new WWPoint(this, 0, to.y());
        adding << add;
        cr8block << PointMemento(add->x, add->y, -1);

        add = new WWPoint(this, to.x(), to.y());
        adding << add;
        cr8block << PointMemento(add->x, add->y, -1);

    } else {

        // appending ?
        if (points_.last()->x < to.x()) {

            //
            // Append a block accounting for trailing end of workout
            //

            // we should really just add two points as the last
            // point will be our 'bottom left' (forgetting for a
            // moment that we go below or above).
            //
            // but we will need to add 4 points not 2 if
            // a) we are above the last point and it is directly
            //    below the previous point (i.e. vertical drop)
            // b) we are below the last point and it is directly
            //    above the previous point (i.e. vertical raise
            //
            // So lets work out what the last point is doing
            enum { notvert, down, up } direction = notvert;
            WWPoint *last = points_.last();
            if (points_.count() > 1) {

                // WEIRD: using i as temp variable to workaround weird
                // macro expansion issue in qglobal.h (!!)
                unsigned int i = points_.count(); i -=2;
                WWPoint *prev = points_[i];

                if (last->x == prev->x) {
                    if (last->y > prev->y) direction = up;
                    if (last->y < prev->y) direction = down;
                }
            }

            switch(direction) {

                case notvert:
                    // not vert just add 2 points
                    add = new WWPoint(this, last->x, to.y());
                    adding << add;
                    cr8block << PointMemento(add->x, add->y, -1);
                    add = new WWPoint(this, to.x(), to.y());
                    adding << add;
                    cr8block << PointMemento(add->x, add->y, -1);
                    break;

                default:
                case down:
                case up:
                    // add a right angle, since that is
                    // consistent to what they have
                    add = new WWPoint(this, to.x(), last->y);
                    adding << add;
                    cr8block << PointMemento(add->x, add->y, -1);
                    add = new WWPoint(this, to.x(), to.y());
                    adding << add;
                    cr8block << PointMemento(add->x, add->y, -1);

                    break;
            }

        } else {

            //
            // Insert a block between points and handle slopes gracefully
            //

            // we are between two points, so the point that
            // was clicked by the user is the MIDDLE of the block
            int prev=-1, next=-1;
            for(int i=0; i < points_.count(); i++) {
                WWPoint *p = points_[i];
                if (p->x < to.x()) prev=i; // to left
                if (p->x >= to.x()) {
                    next=i;
                    break;
                }
            }

            // not between?
            if (prev == -1 || next == -1 || prev == next)  return false;

            // directly below?
            if (points_[prev]->x == to.x() || points_[next]->x == to.x()) return false;

            // it will be as wide as the distance from the nearest
            // point divided by 1.5 (1width space, 1width / 2 for centre)
            int left = (to.x() - points_[prev]->x);
            int right = (points_[next]->x - to.x());
            int width = double(left > right ? right : left) / 1.5;

            // now we know the width we can just add four points
            int index=next;

            // if prev and next are above/below each other then
            // we need to account for that when placing the bottom
            // if the block - ie. place it on a slope
            double ratio = double(points_[next]->y - points_[prev]->y)
                           / double(points_[next]->x - points_[prev]->x);

            // horizontal gap between prev point and lhs and rhs
            double lwidth = to.x() - (width/2) - points_[prev]->x;
            double rwidth = to.x() + (width/2) - points_[prev]->x;

            // bottom left
            add = new WWPoint(this, to.x()-(width/2),
                                    points_[prev]->y + (lwidth * ratio),
                                    false);
            adding << add;
            cr8block << PointMemento(add->x, add->y, index);
            points_.insert(index++, add);

            // top left
            add = new WWPoint(this, to.x()-(width/2), to.y(), false);
            adding << add;
            cr8block << PointMemento(add->x, add->y, index);
            points_.insert(index++, add);

            // top right
            add = new WWPoint(this, to.x()+(width/2), to.y(), false);
            adding << add;
            cr8block << PointMemento(add->x, add->y, index);
            points_.insert(index++, add);

            // bottom right
            add = new WWPoint(this, to.x()+(width/2),
                                    points_[prev]->y + (rwidth * ratio),
                                    false);
            adding << add;
            cr8block << PointMemento(add->x, add->y, index);
            points_.insert(index++, add);

        }
    }

    // did we create any
    if (cr8block.count()) {

        // highlight were we align.
        foreach(WWPoint *point, points_) {
            if (adding.contains(point)) continue;

            point->hover=false;
            if (doubles_equal(point->y, to.y())) point->hover = true;
        }
        return true;
    }
    return false;
}

bool
WorkoutWidget::scale(QPoint p)
{
    // scale selected (all at present) points
    // up of y is positive, down 1% if y is negative
    if (p.y() == 0) return false;

    double factor = p.y() > 0 ? 1.01 : 0.99;

    // scale
    foreach (WWPoint *p, points_) p->y *= factor;

    // register command
    new ScaleCommand(this, 1.01, 0.99, p.y() > 0);

    return true;
}

bool
WorkoutWidget::deleteSelected()
{
    // get a work list for the command then delete them backwards
    QList<PointMemento> list;
    for(int i=0; i<points_.count(); i++) {
        WWPoint *p = points_[i];
        if (p->selected) list << PointMemento(p->x, p->y, i);
    }

    // run the command instead of duplicating here :)
    for (int j=list.count()-1; j>=0; j--) {
        PointMemento m = list[j];
        WWPoint *rm = points_.takeAt(m.index);
        delete rm;
    }

    // create a command on stack
    new DeleteWPointsCommand(this, list);

    return true;
}

bool
WorkoutWidget::selectAll()
{
    bool selected=false;

    foreach(WWPoint *p, points_) {

        // if not selected, select it
        if (p->selected==false)
            p->selected=selected=true;
    }
    return selected;
}

bool
WorkoutWidget::selectPoints()
{
    // if points are in rectangle then set selecting
    // if they are not then unset selecting
    QRectF rect(onRect,atRect);
    foreach(WWPoint *p, points_) {

        // experiment with deselecting when using a
        // rect selection tool since more often than
        // not I keep forgetting points are highlighted
        // XXX maybe a keyboard modifier in the future ?
        p->selected=false;

        if (p->bounding().intersects(rect))
            p->selecting = true;
        else
            p->selecting = false;
    }
    return true;
}

bool
WorkoutWidget::selectedPoints()
{
    // any points marked as selecting are now selected
    foreach(WWPoint *p, points_) {
        if (p->selecting) {
            p->selected=true;
            p->selecting=false;
        }
    }
    return true;
}

bool
WorkoutWidget::selectClear()
{
    // clear all selection
    foreach(WWPoint *p, points_) {
        p->selected=false;
        p->selecting=false;
    }
    return true;
}

void
WorkoutWidget::ergFileSelected(ErgFile *ergFile)
{
    // reset state and stack
    state = none;
    dragging = NULL;
    foreach (WorkoutWidgetCommand *p, stack) delete p;
    stack.clear();
    stackptr = 0;
    parent->saveAct->setEnabled(false);
    parent->undoAct->setEnabled(false);
    parent->redoAct->setEnabled(false);
    cursorBlock = selectionBlock = QPainterPath();
    cursorBlockText = selectionBlockText = cursorBlockText2 = selectionBlockText2 = "";
    //XXX consider refactoring this !!! XXX

    // wipe out points
    foreach(WWPoint *point, points_) delete point;
    points_.clear();

    // we suport ERG but not MRC/CRS currently
    if (ergFile && ergFile->format == ERG) {

        this->ergFile = ergFile;

        maxX_=0;
        maxY_=400;

        // get laps
        laps_ = ergFile->Laps;

        // add points for this....
        foreach(ErgFilePoint point, ergFile->Points) {
            WWPoint *add = new WWPoint(this, point.x / 1000.0f, point.y); // in ms
            if (add->x > maxX_) maxX_ = add->x;
            if (add->y > maxY_) maxY_ = add->y;
        }

        maxY_ *= 1.1f;

    } else {

        // not supported
        ergFile = NULL;
    }

    // reset metrics etc
    recompute();

    // repaint
    repaint();
}

void
WorkoutWidget::recompute()
{
    //QTime timer;
    //timer.start();

    //
    // As data changes so must the selection/cursor
    //
    setBlockCursor();

    int rnum=-1;
    if (context->athlete->zones(false) == NULL ||
        (rnum = context->athlete->zones(false)->whichRange(QDate::currentDate())) == -1) {

        // no cp or ftp set
        parent->TSSlabel->setText("- TSS");
        parent->IFlabel->setText("- IF");

    }

    //
    // PREPARE DATA
    //

    // get CP/FTP to use in calculation
    int WPRIME = context->athlete->zones(false)->getWprime(rnum);
    int CP = context->athlete->zones(false)->getCP(rnum);
    int FTP = context->athlete->zones(false)->getFTP(rnum);
    bool useCPForFTP = (appsettings->cvalue(context->athlete->cyclist,
                        context->athlete->zones(false)->useCPforFTPSetting(), 0).toInt() == 0);
    if (useCPForFTP) FTP=CP;

    // truncate
    wattsArray.resize(0);
    mmpArray.resize(0);

    // running time and watts for interpolating
    int ctime = 0;
    double cwatts = 0;

    double maxy=0;

    // resample the erg file into 1s samples
    foreach(WWPoint *p, points_) {

        // ramprate per second
        double ramp = double(p->y - cwatts) / double(p->x - ctime);

        while (ctime < p->x) {
            cwatts += ramp;
            ctime++;
            wattsArray << int(cwatts);
        }

        cwatts = p->y;
        if (cwatts > maxy) maxy=cwatts;
    }

    // rescale the yaxis
    if (maxY_ > (maxy*2) && maxY_ > 400) maxY_ = maxy *1.5; // too big
    if (maxY_ < maxy) maxY_ = maxy *1.5; // too small
    if (maxy == 0) maxY_ = 400;

    //
    // COMPUTE KEY METRICS TSS/IF
    //

    // The Workout Window has labels for TSS and IF.
    double NP=0, TSS=0, IF=0;

    // calculating NP
    QVector<int> NProlling(30);
    NProlling.fill(0,30);
    double NPtotal=0;
    double NPsum=0;
    int NPindex=0;
    int NPcount=0;

    foreach(int watts, wattsArray) {

        //
        // Normalised Power
        //

        // sum last 30secs
        NPsum += watts;
        NPsum -= NProlling[NPindex];
        NProlling[NPindex] = watts;

        // running total and count
        NPtotal += pow(NPsum/30,4); // raise rolling average to 4th power
        NPcount ++;

        // it moves up and down during the ride
        if (NPcount > 30) {
            NP = pow(double(NPtotal) / double(NPcount), 0.25f);
        }

        // move index on/round
        NPindex = (NPindex >= 29) ? 0 : NPindex+1;
    }

    // IF.....
    IF = double(NP) / double(FTP);

    // TSS.....
    double normWork = NP * NPcount;
    double rawTSS = normWork * IF;
    double workInAnHourAtCP = FTP * 3600;
    TSS = rawTSS / workInAnHourAtCP * 100.0;

    parent->IFlabel->setText(QString("%1 IF").arg(IF, 0, 'f', 2));
    parent->TSSlabel->setText(QString("%1 TSS").arg(TSS, 0, 'f', 0));

    //
    // COMPUTE W'BAL
    //
    wpBal.setWatts(context, wattsArray, CP, WPRIME);

    //
    // MEAN MAX [works but need to think about UI]
    //
    RideFileCache::fastSearch(wattsArray, mmpArray, mmpOffsets);
    //qDebug()<<"RECOMPUTE:"<<timer.elapsed()<<"ms"<<wattsArray.count()<<"samples";

    //
    // SEARCH FOR IMPOSSIBLE TTE SECTIONS
    //
    QVector<long> integrated;
    integrated.resize(wattsArray.size());
    long rt=0;
    int secs=wattsArray.size();
    for(int i=0; i<wattsArray.size(); i++) {
        rt += wattsArray[i];
        integrated[i] = rt;
    }

    // clear what we found last time
    efforts.clear();

    for (int j=0; j<2; j++) {

        // 2 iterations:- 85% sustained, then 100% or higher
        WWEffort tte; tte.start = tte.duration = 0;

        for (int i=0; i<secs; i++) {

            // start out at 30 minutes and drop back to
            // 2 minutes, anything shorter and we are done
            int t = (secs-i-1) > 3600 ? 3600 : secs-i-1;

            while (t > 120) {

                // calculate the TTE for the joules in the interval
                // starting at i seconds with duration t
                // This takes the monod equation p(t) = W'/t + CP and
                // solves for t, but the added complication of also
                // accounting for the fact it is expressed in joules
                // So take Joules = (W'/t + CP) * t and solving that
                // for t gives t = (Joules - W') / CP
                double tc = ((integrated[i+t]-integrated[i]) - WPRIME) / CP;
                // NOTE FOR ABOVE: it is looking at accumulation AFTER this point
                //                 not FROM this point, so we are looking 1s ahead of i
                //                 which is why the interval is registered as starting
                //                 at i+1 in the code below

                // this is either a TTE or getting very close
                if (tc >= (j ? t : (t*0.85))) {

                    if (tte.start > (i+1) || (tte.duration+tte.start) < (i+t)) {

                        tte.start = i + 1; // see NOTE above
                        tte.duration = t;
                        tte.joules = integrated[i+t]-integrated[i];
                        tte.quality = tc / double(t);

                        // add 100 or more on second round
                        // quick way of doing overlapping
                        if ((j && tc >= t) || (!j && tc < t)) efforts << tte;
                    }


                    // move on shorter/harder are just as bad
                    t=0;

                } else {

                    t = tc;
                    if (t<120)
                        t=120;
                }
            }
        }
    }
}

void
WorkoutWidget::configChanged(qint32)
{
    setProperty("color", GColor(CTRAINPLOTBACKGROUND));

    gridPen = QPen(GColor(CPLOTGRID));
    gridPen.setWidth(1);

    markerPen = QPen(GColor(CPLOTMARKER));
    markerPen.setWidth(1);

    markerFont.fromString(appsettings->value(this, GC_FONT_CHARTLABELS, QFont().toString()).toString());
    markerFont.setPointSize(appsettings->value(NULL, GC_FONT_CHARTLABELS_SIZE, 8).toInt());

    bigFont = markerFont;
    bigFont.setPointSize(markerFont.pointSize() * 2);
    bigFont.setWeight(QFont::Bold);

    repaint();
}

void
WorkoutWidget::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.save();

    // use antialiasing when drawing
    painter.setRenderHint(QPainter::Antialiasing, true);

    // just for debug for now
    //DEBUG painter.setPen(QPen(QColor(255,255,255,30)));
    //DEBUG painter.drawRect(left());
    //DEBUG painter.drawRect(right());
    //DEBUG painter.drawRect(top());
    //DEBUG painter.drawRect(bottom());
    //DEBUG painter.setPen(QPen(QColor(255,0,0,30)));
    //DEBUG painter.drawRect(canvas());

    //
    // SCALES [refactor later]
    //

    // paint the scale backbones
    painter.setPen(markerPen);
    painter.setFont(markerFont);
    QFontMetrics fontMetrics(markerFont);

    // backbone
    if (YTICLENGTH) painter.drawLine(left().topRight(), left().bottomRight()); //Y
    if (XTICLENGTH) painter.drawLine(bottom().topLeft(), bottom().topRight()); //X

    // start with 5 min tics and get longer and longer
    int tsecs = 5 * 60; // 5 minute tics
    int xrange = maxX() - minX();
    while (double(xrange) / double(tsecs) > XTICS) tsecs *= 2;

    // now paint them
    for(int i=minX(); i<=maxX(); i += tsecs) {

        painter.setPen(markerPen);

        // paint a tic
        int x= transform(i, 0).x();

        if (XTICLENGTH) { // we can make the tics disappear

            painter.drawLine(QPoint(x, bottom().topLeft().y()),
                             QPoint(x, bottom().topLeft().y()+XTICLENGTH));
        }

        // always paint the label
        QString label = time_to_string(i);
        QRect bound = fontMetrics.boundingRect(label);
        painter.drawText(QPoint(x - (bound.width() / 2),
                                bottom().topLeft().y()+fontMetrics.ascent()+XTICLENGTH+(XTICLENGTH ? SPACING : 0)),
                                label);

    }

    // Y-SCALE (absolute shown on canvas next to grid lines)
    QString label = tr("Intensity");
    QRect box = fontMetrics.boundingRect(label);
    painter.rotate(-90);
    painter.setPen(markerPen);
    //painter.drawText(QPoint(left().x()+box.height(), left().center().y()+(box.width()/2)), label);
    painter.drawText(0,box.height(),label);
    painter.rotate(90);

    // start with 50w tics and get longer and longer
    int twatts = 50; // 50w tics
    int yrange = maxY() - minY();
    while (double(yrange) / double(twatts) > YTICS) twatts *= 2;

    // now paint them
    for(int i=minY(); i<=maxY(); i += twatts) {

        painter.setPen(markerPen);

        // paint a tic
        int y= transform(0, i).y();

        if (YTICLENGTH) { // we can make the tics disappear

        painter.drawLine(QPoint(left().topRight().x(), y),
                         QPoint(left().topRight().x()-YTICLENGTH, y));

        }

        // always paint the watts as a reference
        if (GRIDLINES && i>0) {
            painter.setPen(markerPen);

            QString label = QString("%1w").arg(i);
            painter.drawText(QPoint(canvas().left()+SPACING,
                                    y+(fontMetrics.ascent()/2)), // we use ascent not height to line up numbers
                                    label);

#if 0       // ONLY SHOW GRIDLINES FROM POWERSCALE
            // GRIDLINES - but not on top and bottom border of canvas
            if (y > canvas().y() && y < canvas().height() && GRIDLINES == true) {
                painter.setPen(gridPen);
                painter.drawLine(QPoint(canvas().x(), y), QPoint(canvas().x()+canvas().width(), y));
            }
#endif
        }
    }

    // now paint the scene and related widgets
    foreach(WorkoutWidgetItem*x, children_) x->paint(&painter);

    // now paint the points
    foreach(WorkoutWidgetItem*x, points_) x->paint(&painter);

    painter.restore();
}

QRectF
WorkoutWidget::left()
{
    QRect all = geometry();
    return QRectF(0,THEIGHT, LWIDTH, all.height() - IHEIGHT - THEIGHT - BHEIGHT);
}

QRectF
WorkoutWidget::right()
{
    QRect all = geometry();
    return QRectF(all.width()-RWIDTH,THEIGHT, RWIDTH, all.height() - IHEIGHT - THEIGHT - BHEIGHT);
}

QRectF
WorkoutWidget::bottom()
{
    QRect all = geometry();
    return QRectF(LWIDTH, all.height() - BHEIGHT, all.width() - LWIDTH - RWIDTH, BHEIGHT);
}

QRectF
WorkoutWidget::bottomgap()
{
    QRect all = geometry();
    return QRectF(LWIDTH, all.height() - (IHEIGHT+BHEIGHT), all.width() - LWIDTH - RWIDTH, IHEIGHT);
}

QRectF
WorkoutWidget::top()
{
    QRect all = geometry();
    return QRectF(LWIDTH, 0, all.width() - LWIDTH - RWIDTH, THEIGHT);
}

QRectF
WorkoutWidget::canvas()
{
    QRect all = geometry();
    return QRectF(LWIDTH, THEIGHT, all.width() - LWIDTH - RWIDTH, all.height() - IHEIGHT - THEIGHT - BHEIGHT);
}

WorkoutWidgetItem::WorkoutWidgetItem(WorkoutWidget *w) : w(w)
{
}

double
WorkoutWidget::maxX()
{
    return maxX_;
}

double
WorkoutWidget::maxY()
{
    return maxY_;
}

// transform from plot to painter co-ordinate
QPoint
WorkoutWidget::transform(double seconds, double watts, RideFile::SeriesType s)
{
    // from plot coords to painter coords on the canvas
    QRectF c = canvas();

    switch (s) {

    default:
    case RideFile::watts:
        {
        // ratio of pixels to plot units
        double yratio = double(c.height()) / (maxY()-minY());
        double xratio = double(c.width()) / (maxX()-minX());

        return QPoint(c.x() + (seconds * xratio), c.bottomLeft().y() - (watts * yratio));
        }

    case RideFile::hr:
        {
        // ratio of pixels to plot units
        double yratio = double(c.height()) / double(hrMax);
        double xratio = double(c.width()) / (maxX()-minX());

        return QPoint(c.x() + (seconds * xratio), c.bottomLeft().y() - (watts * yratio));
        }
    case RideFile::cad:
        {
        // ratio of pixels to plot units
        double yratio = double(c.height()) / double(cadenceMax);
        double xratio = double(c.width()) / (maxX()-minX());

        return QPoint(c.x() + (seconds * xratio), c.bottomLeft().y() - (watts * yratio));
        }
    case RideFile::kph:
        {
        // ratio of pixels to plot units
        double yratio = double(c.height()) / double(speedMax);
        double xratio = double(c.width()) / (maxX()-minX());

        return QPoint(c.x() + (seconds * xratio), c.bottomLeft().y() - (watts * yratio));
        }
    }
}

// transform from painter to plot co-ordinate
QPointF
WorkoutWidget::reverseTransform(int x, int y)
{
    // from painter coords to plot cords on the canvas
    QRectF c = canvas();

    // ratio of pixels to plot units
    double yratio = double(c.height()) / (maxY()-minY());
    double xratio = double(c.width()) / (maxX()-minX());

    return QPoint((x-c.x()) / xratio, (c.bottomLeft().y() - y) / yratio);
}

WorkoutWidgetCommand::WorkoutWidgetCommand(WorkoutWidget *w) : workoutWidget_(w)
{
    // add us
    w->addCommand(this);
}

// add to the stack, don't execute since it was already executed
// and this is a memento to enable undo / redo - its a trigger
// to recompute metrics though since the model has been changed
void
WorkoutWidget::addCommand(WorkoutWidgetCommand *cmd)
{
    // stop dragging
    dragging = NULL;
    state = none;

    // truncate if needed
    if (stack.count()) {
        // wipe away commands we can no longer redo
        while (stack.count() > stackptr) {
            WorkoutWidgetCommand *p = stack.takeAt(stackptr);
            delete p;
        }
    }

    // add to stack
    stack.append(cmd);
    stackptr++;

    // set undo enabled, redo disabled
    parent->saveAct->setEnabled(true);
    parent->undoAct->setEnabled(true);
    parent->redoAct->setEnabled(false);
}

void
WorkoutWidget::redo()
{
    // stop dragging
    dragging = NULL;
    state = none;

    // redo if we can
    if (stackptr >= 0 && stackptr < stack.count()) stack[stackptr++]->redo();

    // disable/enable buttons
    if (stackptr > 0) {
        parent->undoAct->setEnabled(true);
        parent->saveAct->setEnabled(true);
    }
    if (stackptr >= stack.count()) parent->redoAct->setEnabled(false);

    // recompute metrics
    recompute();

    // update
    update();
}

void
WorkoutWidget::undo()
{
    // stop dragging
    dragging = NULL;
    state = none;

    // run it
    if (stackptr > 0) stack[--stackptr]->undo();

    // disable/enable button
    if (stackptr <= 0) {
        parent->saveAct->setEnabled(false);
        parent->undoAct->setEnabled(false);
    }
    if (stackptr < stack.count()) parent->redoAct->setEnabled(true);

    // recompute metrics
    recompute();

    // update
    update();
}

// if no block is highlighted return false, otherwise true
bool
WorkoutWidget::getBlockSelected(QList<int>&copyIndexes,
                                QList<int>&deleteIndexes,
                                double &shift)
{
    // from first to last selected these are in scope
    int begin=-1, end=-1;
    for(int i=0; i<points_.count(); i++) {
        if (points_[i]->selected) {
            if (begin == -1) begin = i;
            end = i;
        }
    }

    // make sure there is some kind of selection
    if (begin == -1 || end == -1 || begin == end || end < begin) return false;

    // trim unwanted points from the front
    int blockStarts=-1, blockFinishes=-1;

    // j loops through the indexes of the points in scope
    for(int index=begin; index<=end; index++) {

        // index of this, next and previous points
        int next = index < end ? index+1 : -1;

        // we got to block end
        if (next != -1 && points_[next]->x > points_[index]->x) {
            blockStarts = index;
            break;
        }
    }

    // j loops back through the indexes of the points in scope
    for(int index=end; index>=begin; index--) {

        // index of this, next and previous points
        int prev = index > begin ? (index-1) : - 1;

        // we got to block end
        if (prev != -1 && points_[prev]->x < points_[index]->x) {
            blockFinishes = index;
            break;
        }
    }

    if (blockStarts == -1 || blockFinishes == -1 || blockStarts > blockFinishes) return false;

    // what is the shift required to move points
    // to the right across in a cut operation
    shift = points_[blockFinishes]->x - points_[blockStarts]->x;

    // what are the indexes we will put into the
    // buffer in a copy operation
    for(int i=blockStarts; i<=blockFinishes; i++)
        copyIndexes << i;

    // what are the indexes we would delete in
    // a cut operation - for now same as copy
    // but should really get rid of duplicate
    // points esp. when multiple points for the
    // same point in time XXX fix this later XXX
    // as a result of shifting this happens a
    // fair amount ............
    deleteIndexes = copyIndexes;

    // do we have something ?
    if (copyIndexes.count() > 1) return true;

    return false;
}

void
WorkoutWidget::cut()
{
    QList<PointMemento>d,c;

    QList<int> copyIndexes, deleteIndexes;
    double shift=0;

    // work out what we're doing
    if (!getBlockSelected(copyIndexes, deleteIndexes, shift)) return;

    // we cut and paste BLOCKS, not POINTS
    // so this means we have to only get points
    // that are part of the block we are cutting
    // and also shift the points to the right
    // across to the left to account for the
    // blocks we removed.

    // since copy also needs to do this we have
    // the following utility

    // copy points
    foreach(int index, copyIndexes) {
        WWPoint *p = points_[index];
        c << PointMemento(p->x, p->y, index);
    }
    setClipboard(c);

    // delete backwards
    int last=-1;
    for(int i=deleteIndexes.count()-1; i>=0; i--) {
        WWPoint *take = points_.takeAt(deleteIndexes[i]);
        d << PointMemento(take->x, take->y, deleteIndexes[i]);
        delete take;
        last = deleteIndexes[i];
    }

    // now shift the rest
    for(int i=last; i >0 && i<points_.count(); i++) {
        points_[i]->x -= shift;
    }

    // add the cut command
    new CutCommand(this, c, d, shift);

    // refresh
    recompute();

    // update the display
    update();
}

void
WorkoutWidget::copy()
{
    QList<PointMemento>d,c;

    // to use getBlockSelected
    QList<int> copyIndexes, deleteIndexes;
    double shift=0;

    // work out what we're doing
    if (!getBlockSelected(copyIndexes, deleteIndexes, shift)) return;

    // copy points
    foreach(int index, copyIndexes) {
        WWPoint *p = points_[index];
        c << PointMemento(p->x, p->y, index);
    }
    setClipboard(c);
}

void
WorkoutWidget::setClipboard(QList<PointMemento>&c)
{
    // always offset from zero
    // for both time and indexes
    double offset=0;
    for(int i=0; i<c.count(); i++) {
        if (i==0) offset=c[i].x;
        c[i].x -= offset; // time starts from zero
        c[i].index = i;   // indexes start from zero
    }

    // store it and set the action button
    clipboard = c;
    parent->pasteAct->setEnabled(c.count() > 0);
}

void
WorkoutWidget::paste()
{
    // empty clipboard, nothing to do
    if (clipboard.count() == 0) return;

    // ok, so where to paste?
    int here=-1;
    int offset= points_.count() ? points_.last()->x : 0;

    // search for a selected point
    for(int i=points_.count()-1; i>=0; i--) {
        if (points_[i]->selected) {
            offset=points_[i]->x;
            here=i+1;
            break;
        }
    }

    // if its the last point append!
    if (here >= (points_.count())) here = -1;

    double shift=0;
    if (here != -1) {

        // need to shift everyone across to make space
        shift = clipboard.last().x;

        for(int i=here; i<points_.count(); i++) {
            points_[i]->x += shift;
        }
    }

    // here is either the index to append after
    // or we add to the end of the workout
    foreach(PointMemento m, clipboard) {

        if (here == -1) {
            new WWPoint(this, m.x+offset, m.y);
        } else {

            WWPoint *add = new WWPoint(this, m.x+offset, m.y, false);
            points_.insert(here + m.index, add);
        }
    }

    // increase maxX ?
    if (points_.count() && points_.last()->x > maxX_)
        maxX_ = points_.last()->x;

    // paste command
    new PasteCommand(this, here, offset, shift, clipboard);

    // refresh
    recompute();

    // update the display
    update();
}
