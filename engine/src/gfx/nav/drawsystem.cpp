/*
 * drawsystem.cpp
 *
 * Vega Strike - Space Simulation, Combat and Trading
 * Copyright (C) 2001-2025 The Vega Strike Contributors:
 * Project creator: Daniel Horn
 * Original development team: As listed in the AUTHORS file
 * Current development team: Roy Falk, Benjamen R. Meyer, Stephen G. Tuggy
 *
 *
 * https://github.com/vegastrike/Vega-Strike-Engine-Source
 *
 * This file is part of Vega Strike.
 *
 * Vega Strike is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Vega Strike is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Vega Strike.  If not, see <https://www.gnu.org/licenses/>.
 */


#include <set>
#include "vsfilesystem.h"
#include "vs_globals.h"
#include "vegastrike.h"
#include "gfx/gauge.h"
#include "gfx/cockpit.h"
#include "universe.h"
#include "star_system.h"
#include "cmd/unit_generic.h"
#include "cmd/collection.h"
#include "gfx/hud.h"
#include "gfx/vdu.h"
#include "lin_time.h" //for fps
#include "config_xml.h"
#include "lin_time.h"
#include "cmd/images.h"
#include "cmd/script/mission.h"
#include "cmd/script/msgcenter.h"
#include "cmd/ai/flyjoystick.h"
#include "cmd/ai/firekeyboard.h"
#include "cmd/ai/aggressive.h"
#include "main_loop.h"
#include <assert.h>     //needed for assert() calls
#include "savegame.h"
#include "gfx/animation.h"
#include "gfx/mesh.h"
#include "universe_util.h"
#include "in_mouse.h"
#include "gui/glut_support.h"
#include "cmd/unit_util.h"
#include "math.h"
#include "save_util.h"
#include "navscreen.h"
#include "gfx/masks.h"
#include "galaxy_gen.h"

//**********************************
//Main function for drawing a CURRENT system
//works :
//scans all items, records min + max coords of the system, for relevant items
//rescans, and enlists the found items that it wants drawn
//-	items with mouse over them will go into a mouselist.
//draws the draw lists, with the mouse lists cycled 'n' times (according to kliks)
//**********************************

string getUnitNameAndFgNoBase(Unit *target);   // gfx/vdu.cpp
extern bool TargMission(Unit *me, Unit *target);   // cmd/ai/firekeyboard.cpp

// PGG: is this unit part of an active mission (a mission script called setMissionRelevant on it) whose
// objective naming it is not yet complete? Objectives don't link to units, so match on the unit's name;
// a relevant unit no objective names (e.g. a target ship) counts as outstanding.
static bool IsOutstandingMissionUnit(Unit *un) {
    if (!un || !TargMission(nullptr, un)) {
        return false;
    }
    const string name = getUnitNameAndFgNoBase(un);
    bool named = false;
    for (unsigned int i = 0; i < active_missions.size(); ++i) {
        for (const auto &obj : active_missions[i]->objectives) {
            if (!name.empty() && obj.objective.find(name) != string::npos) {
                if (obj.completeness < 1) {
                    return true;
                }
                named = true;
            }
        }
    }
    return !named;
}

void NavigationSystem::DrawSystem() {
    UniverseUtil::PythonUnitIter bleh = UniverseUtil::getUnitList();
    if (!(*bleh)) {
        return;
    }
//string mystr ("3d "+XMLSupport::tostring (system_view));
//UniverseUtil::IOmessage (0,"game","all",mystr);

    //what's my name
    //***************************
    TextPlane systemname;       //will be used to display shits names
    int faction =
            FactionUtil::GetFactionIndex(UniverseUtil::GetGalaxyFaction(_Universe->activeStarSystem()->getFileName()));
    //GFXColor factioncolor = factioncolours[faction];
    // PGG: 0.9.1 keeps the system's file path ("Gemini/Troy.system") as its name, where 0.5.1 had the XML
    // name ("Troy"); the long path ran the title off the nav grid, so show only the system part.
    string shownname = _Universe->activeStarSystem()->getName();
    string::size_type slash = shownname.find_last_of("/\\");
    if (slash != string::npos) {
        shownname = shownname.substr(slash + 1);
    }
    if (shownname.size() > 7 && shownname.compare(shownname.size() - 7, 7, ".system") == 0) {
        shownname.erase(shownname.size() - 7);
    }
    string systemnamestring =
            "#ff0000Sector: #ffff00" + getStarSystemSector(_Universe->activeStarSystem()->getFileName())
                    + "  #ff0000Current System: #ffff00" + shownname + " ("
                    + FactionUtil::GetFactionName(faction)
                    + "#ffff00)";
    // PGG: say what a click selected (it targets that nav point; autopilot then flies to it), since the map
    // itself gives almost no feedback.
    if (Unit *navparent = _Universe->AccessCockpit()->GetParent()) {
        if (Unit *navtarget = navparent->Target()) {
            string tname = getUnitNameAndFgNoBase(navtarget);
            if (!tname.empty()) {
                systemnamestring += "\n#ff0000Selected: #00ff00" + tname;
            }
        }
    }
    //int length = systemnamestring.size();
    //float offset = (float(length)*0.001);
    //systemname.SetPos( (((screenskipby4[0]+screenskipby4[1])/2)-offset) , screenskipby4[3]); // middle position
    systemname.SetPos(screenskipby4[0] + 0.03, screenskipby4[3] + 0.02);     //left position
    systemname.col = GFXColor(1, 1, .7, 1);
    systemname.SetText(systemnamestring);
//systemname.SetCharSize(1, 1);
    const float background_alpha = configuration()->graphics_config.hud.text_background_alpha;
    GFXColor tpbg = systemname.bgcol;
    bool automatte = (0 == tpbg.a);
    if (automatte) {
        systemname.bgcol = GFXColor(0, 0, 0, background_alpha);
    }
    systemname.Draw(systemnamestring, 0, true, false, automatte);
    systemname.bgcol = tpbg;
    //***************************

//navdrawlist mainlist(0, screenoccupation, factioncolours);		//	lists of items to draw
//mainlist.unselectedalpha = unselectedalpha;
    navdrawlist mouselist(1, screenoccupation, factioncolours);       //lists of items to draw that are in mouse range

    QVector pos;    //item position
    QVector pos_flat;       //item position flat on plane

    float zdistance = 0.0;
    float zscale = 0.0;

    Adjust3dTransformation(system_view == VIEW_3D, 1);
    //Set up first item to compare to + centres
    //**********************************
    while ((*bleh) && (_Universe->AccessCockpit()->GetParent() != (*bleh))
            && (UnitUtil::isSun(*bleh)
                    || !UnitUtil::isSignificant(*bleh))) {                                                                       //no sun's in initial setup
        ++bleh;
    }
    if (!(*bleh)) {      //nothing there that's significant, just do it all
        bleh = UniverseUtil::getUnitList();
    }
    //GET THE POSITION
    //*************************
    pos = (*bleh)->Position();
    ReplaceAxes(pos);
    //*************************

    //Modify by old rotation amount
    //*************************
//if(system_view==VIEW_3D)
//{
//pos = dxyz(pos, 0, ry_s, 0);
//pos = dxyz(pos, rx_s, 0, 0);
//}
    //*************************

    float max_x = (float) pos.i;
    float min_x = (float) pos.i;
    float max_y = (float) pos.j;
    float min_y = (float) pos.j;
    float max_z = (float) pos.k;
    float min_z = (float) pos.k;

//float themaxvalue = fabs(pos.i);
    themaxvalue = 0.0;

    float center_nav_x = ((screenskipby4[0] + screenskipby4[1]) / 2);
    float center_nav_y = ((screenskipby4[2] + screenskipby4[3]) / 2);
    //**********************************
    //Retrieve unit data min/max
    //**********************************
    while (*bleh) {
        //this goes through one time to get the major components locations, and scales its output appropriately
        if (UnitUtil::isSun(*bleh)) {
            ++bleh;
            continue;
        }
        string temp = (*bleh)->name;
        pos = (*bleh)->Position();
        ReplaceAxes(pos);
        //Modify by old rotation amount
        //*************************
//if(system_view==VIEW_3D)
//{
//pos = dxyz(pos, 0, ry_s, 0);
//pos = dxyz(pos, rx_s, 0, 0);
//}
        //*************************
        //*************************
        if ((UnitUtil::isSignificant(*bleh)) || (_Universe->AccessCockpit()->GetParent() == (*bleh))) {
            RecordMinAndMax(pos, min_x, max_x, min_y, max_y, min_z, max_z, themaxvalue);
        }
        ++bleh;
    }
    //**********************************

    //Find Centers
    //**********************************
    center_x = (min_x + max_x) / 2;
    center_y = (min_y + max_y) / 2;
    center_z = (min_z + max_z) / 2;
    //**********************************

    max_x = 2 * max_x - center_x;
    max_y = 2 * max_y - center_y;
    max_z = 2 * max_z - center_z;
    min_x = 2 * min_x - center_x;
    min_y = 2 * min_y - center_y;
    min_z = 2 * min_z - center_z;

    themaxvalue *= 2;

//#define SQRT3 1.7320508
//themaxvalue = sqrt(themaxvalue*themaxvalue + themaxvalue*themaxvalue + themaxvalue*themaxvalue);
//themaxvalue = SQRT3*themaxvalue;

    //Set Camera Distance
    //**********************************
//{
    float half_x = (max_x - min_x);
    float half_y = (max_y - min_y);
    float half_z = (max_z - min_z);

    camera_z = sqrt((half_x * half_x) + (half_y * half_y) + (half_z * half_z));

//float halfmax = 0.5*themaxvalue;
//camera_z = sqrt( (halfmax*halfmax) + (halfmax*halfmax) + (halfmax*halfmax) );
//camera_z = 4.0*themaxvalue;
//}

    //**********************************

    DrawOriginOrientationTri(center_nav_x, center_nav_y, 1);

/*
 *       string mystr ("max x "+XMLSupport::tostring (max_x));
 *       UniverseUtil::IOmessage (0,"game","all",mystr);
 *
 *       string mystr2 ("min x "+XMLSupport::tostring (min_x));
 *       UniverseUtil::IOmessage (0,"game","all",mystr2);
 *
 *       string mystr3 ("max y "+XMLSupport::tostring (max_y));
 *       UniverseUtil::IOmessage (0,"game","all",mystr3);
 *
 *       string mystr4 ("min y "+XMLSupport::tostring (min_y));
 *       UniverseUtil::IOmessage (0,"game","all",mystr4);
 *
 *       string mystrcx ("center x "+XMLSupport::tostring (center_x));
 *       UniverseUtil::IOmessage (0,"game","all",mystrcx);
 *
 *       string mystrcy ("center y "+XMLSupport::tostring (center_y));
 *       UniverseUtil::IOmessage (0,"game","all",mystrcy);
 */

    Unit *ThePlayer = (UniverseUtil::getPlayerX(UniverseUtil::getCurrentPlayer()));

    //Enlist the items and attributes
    //**********************************
    un_iter blah = UniverseUtil::getUnitList();
    while (*blah) {
        //this draws the points

        //Retrieve unit data
        //**********************************
        string temp = (*blah)->name;

        pos = (*blah)->Position();
        ReplaceAxes(pos);

        float the_x, the_y, the_x_flat, the_y_flat, system_item_scale_temp;
        TranslateCoordinates(pos,
                pos_flat,
                center_nav_x,
                center_nav_y,
                themaxvalue,
                zscale,
                zdistance,
                the_x,
                the_y,
                the_x_flat,
                the_y_flat,
                system_item_scale_temp,
                1);
        //IGNORE OFF SCREEN
        //**********************************
        if (!TestIfInRange(screenskipby4[0], screenskipby4[1], screenskipby4[2], screenskipby4[3], the_x, the_y)) {
            ++blah;
            continue;
        }
        //**********************************

        //Now starts the test that determines the type of things and inserts
        //|
        //|
        //\/

        float insert_size = 0.0;
        int insert_type = navambiguous;
        if ((*blah)->isUnit() == Vega_UnitType::unit) {
            //unit
            /*if(UnitUtil::isPlayerStarship(*blah) > -1)	//	is a PLAYER SHIP
             *  {
             *       if (UnitUtil::isPlayerStarship (*blah)==UniverseUtil::getCurrentPlayer()) //	is THE PLAYER
             *       {
             *               insert_type = navcurrentplayer;
             *               insert_size = navcurrentplayersize;
             *       }
             *       else	//	is A PLAYER
             *       {
             *               insert_type = navplayer;
             *               insert_size = navplayersize;
             *       }
             *  }
             *  else	//	is a non player ship
             *  {*/
            if (UnitUtil::isSignificant(*blah)) {
                //capship or station
                if ((*blah)->GetComputerData().max_speed() == 0) {
                    //is this item STATIONARY?
                    insert_type = navstation;
                    insert_size = navstationsize;
                } else {
                    //it moves = capship
                    if (ThePlayer->InRange((*blah), false, false)) {
                        //only insert if in range
                        insert_type = navcapship;
                        insert_size = navcapshipsize;
                    } else {
                        //skip unit completely if not in range
                        ++blah;
                        continue;
                    }
                }
            } else {
                //fighter
                /*if(ThePlayer->InRange((*blah),false,false))	//	only insert if in range
                 *  {
                 *       insert_type = navfighter;
                 *       insert_size = navfightersize;
                 *  }
                 *  else	// skip unit completely if not in range
                 *  {
                 * ++blah;
                 *       continue;
                 *  }*/
                if (UnitUtil::isPlayerStarship(*blah) > -1) {
                    //is THE PLAYER
                    insert_type = navfighter;
                    insert_size = navfightersize;
                } else {
                    //skip unit completely if not in range
                    ++blah;
                    continue;
                }
            }
            //}
        } else if ((*blah)->isUnit() == Vega_UnitType::planet) {
            //is it a PLANET?
            if (UnitUtil::isSun(*blah)) {
                //is this a SUN?
                insert_type = navsun;
                insert_size = navsunsize;
            } else if (!((*blah)->GetDestinations().empty())) {
                //is a jump point (has destinations)
                insert_type = navjump;
                insert_size = navjumpsize;
            } else {
                //its a planet
                insert_type = navplanet;
                insert_size = navplanetsize;
            }
        } else if ((*blah)->isUnit() == Vega_UnitType::missile) {
            //a missile
            insert_type = navmissile;
            insert_size = navmissilesize;
        } else if ((*blah)->isUnit() == Vega_UnitType::asteroid) {
            //an asteroid
            insert_type = navasteroid;
            insert_size = navasteroidsize;
        } else if ((*blah)->isUnit() == Vega_UnitType::nebula) {
            //a nebula
            insert_type = navnebula;
            insert_size = navnebulasize;
        } else {
            //undefined non unit
            insert_type = navambiguous;
            insert_size = navambiguoussize;
        }
        if (system_item_scale_temp > (system_item_scale * 3)) {
            system_item_scale_temp = (system_item_scale * 3);
        }
        insert_size *= system_item_scale_temp;
        // PGG: Privateer marked nav points with an outstanding mission objective in red and the selected one in
        // yellow (the stock target colour was red, which would clash).
        if (IsOutstandingMissionUnit(*blah)) {
            static GFXColor mcol = vs_config->getColor("nav", "mission_unit", GFXColor(1, 0.15, 0.15, 1));
            DrawCircle(the_x, the_y, insert_size * 1.6f, mcol);
            DrawCircle(the_x, the_y, insert_size * 1.75f, mcol);
        }
        if (_Universe->AccessCockpit()->GetParent()->Target() == (*blah)) {
            //Get a color from the config
            static GFXColor col = vs_config->getColor("nav", "targetted_unit", GFXColor(1, 1, 0.2, 0.9));
            DrawTargetCorners(the_x, the_y, insert_size, col);
        }
        bool tests_in_range = 0;
        // PGG: the icons are tiny, so give each a larger mouse hotspot (graphics/nav_click_radius_scale).
        static float click_scale =
                XMLSupport::parse_float(vs_config->getVariable("graphics", "nav_click_radius_scale", "2.5"));
        if (insert_type == navstation) {
            tests_in_range = TestIfInRangeBlk(the_x, the_y, insert_size * click_scale, mouse_x_current, mouse_y_current);
        } else {
            tests_in_range = TestIfInRangeRad(the_x, the_y, insert_size * click_scale, mouse_x_current, mouse_y_current);
        }
        Unit *myunit = (*blah);

        ++blah;
        DisplayOrientationLines(the_x, the_y, the_x_flat, the_y_flat, 1);
        if (tests_in_range) {
            mouselist.insert(insert_type, insert_size, the_x, the_y, myunit);
        } else {
            drawlistitem(insert_type,
                    insert_size,
                    the_x,
                    the_y,
                    myunit,
                    screenoccupation,
                    false,
                    (*blah) ? true : false,
                    unselectedalpha,
                    factioncolours);
        }
    }
    //**********************************	//	done enlisting items and attributes
    //Adjust mouse list for 'n' kliks
    //**********************************
    //STANDARD	: (1 3 2) ~ [0] [2] [1]
    //VS			: (1 2 3) ~ [0] [1] [2]	<-- use this
    if (mouselist.get_n_contents() > 0) {
        //mouse is over a target when this is > 0
        if (mouse_wentdown[2] == 1) {             //mouse button went down for mouse button 2(standard)
            rotations += 1;
        }
    }
    if (rotations >= mouselist.get_n_contents()) {      //dont rotate more than there is
        rotations = 0;
    }
    int r = 0;
    while (r < rotations) {
        //rotate whatver rotations, leaving n rotated items, tail on top
        mouselist.rotate();
        r += 1;
    }
    //**********************************
    //Draw the damn shit
    //**********************************
//mainlist.draw();	//	draw the items
//mainlist.wipe();	//	whipe the list
    //**********************************
    //Check for selection query
    //give back the selected tail IF there is one
    //IF given back, undo the selection state
    //**********************************
    if (1 || checkbit(buttonstates, 1)) {
        //button #2 is down, wanting a (selection)
        if (mouselist.get_n_contents() > 0) {
            //mouse is over a target when this is > 0
            if (mouse_wentdown[0] == 1) {
                //mouse button went down for mouse button 1
                currentselection = mouselist.gettailunit();
                unsetbit(buttonstates, 1);
                //JUST FOR NOW, target == current selection. later it'll be used for other shit, that will then set target.
                if (currentselection.GetUnit()) {
                    (UniverseUtil::getPlayerX(UniverseUtil::getCurrentPlayer()))->Target(currentselection.GetUnit());
                    (UniverseUtil::getPlayerX(UniverseUtil::getCurrentPlayer()))->LockTarget(currentselection.GetUnit());
                }
            }
        }
    }
    //**********************************

    //Clear the lists
    //**********************************
    mouselist.draw();           //draw mouse over'd items
    mouselist.wipe();           //whipe mouse over'd list
    //**********************************
}
//**********************************

