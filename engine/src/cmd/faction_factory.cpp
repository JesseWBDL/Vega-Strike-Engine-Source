/*
 * faction_factory.cpp
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


#include "faction_factory.h"
#include "faction_generic.h"
#include "gfx/animation.h"
#include "cmd/ai/communication.h"
#include "vsfilesystem.h"

#include <map>
#include <cstring>

#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/xml_parser.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/foreach.hpp>
#include <boost/optional/optional.hpp>

namespace pt = boost::property_tree;
namespace alg = boost::algorithm;

using boost::property_tree::ptree;
using boost::optional;

using std::string;

extern std::vector<boost::shared_ptr<Faction> > factions;   //the factions
extern std::vector<std::string> contrabandlists;            // faction_xml.cpp, one entry per faction

FactionFactory::FactionFactory(std::string filename) {
    pt::ptree tree;
    pt::read_xml(filename, tree);

    // Iterate over root
    for (const auto &iterator : tree) {
        parse(iterator.second);

        // There should be only one root. Exiting
        break;
    }
}

void FactionFactory::parse(ptree tree) {
    int counter = 0;
    std::map<string, boost::shared_ptr<FSM> > conversations;   // each conversation file is loaded once
    for (const auto &iterator : tree) {
        if (iterator.first != "Faction") {
            continue;
        }

        ptree inner = iterator.second;
        boost::shared_ptr<Faction> faction(new Faction());

        string tmp_name = inner.get("<xmlattr>.name", "Unknown");

        // TODO: make factioname an std::string
        faction->factionname = new char[tmp_name.size() + 1];
        strcpy(faction->factionname, tmp_name.c_str());

        faction->citizen = inner.get("<xmlattr>.citizen", false);
        string logo_rgb = inner.get("<xmlattr>.logoRGB", "");
        string logo_sec_rgb = inner.get("<xmlattr>.secLogoRGB", "");
        string contraband = inner.get("<xmlattr>.contraband", "");

        // Parse spark colors
        std::string colors[] = {"Red", "Green", "Blue", "Alpha"};
        int i = 0;
        for (const auto &color : colors) {
            if (boost::optional<float> spark = inner.get_optional<float>("<xmlattr>.Spark" + color)) {
                faction->sparkcolor[i++] = *spark;
            }
        }

        string explosion_name = inner.get("Explosion.<xmlattr>.name", "");
        if (!explosion_name.empty()) {
            boost::shared_ptr<Animation> explosion(FactionUtil::createAnimation(explosion_name.c_str()));
            faction->explosion_name.push_back(explosion_name);
            faction->explosion.push_back(explosion);
        }

        for (const auto &iterator : inner) {
            string type = iterator.first;
            ptree inner = iterator.second;

            // We skip attributes and explosions as we processed them above.
            if (type == "<xmlattr>" || type == "Explosion") {
                continue;
            }

            if (type == "ShipModifier") {
                // Per ship-type attitude (e.g. retro towards "talon"), added on top of the faction relation.
                // Kept separate from <stats>, as 0.5.1 did; zero entries are skipped like the old parser.
                string name = inner.get("<xmlattr>.name", "");
                float relation = inner.get("<xmlattr>.relation", 0.0f);
                if (relation != 0 && !name.empty()) {
                    faction->ship_relation_modifier[name] = relation;
                }
                continue;
            }

            if (type == "CommAnimation") {
                // The faces shown on the comm screen when this faction's pilots talk (0.5.1 faction_xml.cpp).
                faction->comm_faces.push_back(Faction::comm_face_t());
                faction->comm_face_sex.push_back((unsigned char) inner.get("<xmlattr>.sex", 0));
                if (boost::optional<string> dock = inner.get_optional<string>("<xmlattr>.dockable_only")) {
                    faction->comm_faces.back().dockable = XMLSupport::parse_bool(*dock)
                            ? Faction::comm_face_t::CYES : Faction::comm_face_t::CNO;
                }
                if (boost::optional<string> base = inner.get_optional<string>("<xmlattr>.base_only")) {
                    faction->comm_faces.back().base = XMLSupport::parse_bool(*base)
                            ? Faction::comm_face_t::CYES : Faction::comm_face_t::CNO;
                }
                for (const auto &mood : inner) {
                    if (mood.first == "MoodAnimation") {
                        string ani = mood.second.get("<xmlattr>.name", "");
                        if (!ani.empty()) {
                            faction->comm_faces.back().animations.push_back(FactionUtil::createAnimation(ani.c_str()));
                        }
                    }
                }
                continue;
            }

            if (type == "stats") {
                string name = inner.get("<xmlattr>.name", "Unknown");
                // Relations are fractions (e.g. -.5); reading them as int turned them all into 0.
                float relation = inner.get("<xmlattr>.relation", 0.0f);

                // My attitude towards faction <name>, and the conversation used with it
                // (e.g. conversation="privtomining.xml"). Indexed by ParseAllies.
                Faction::faction_stuff stuff;
                stuff.stats.name = new char[name.size() + 1];
                strcpy(stuff.stats.name, name.c_str());
                stuff.relationship = relation;
                string conversation = inner.get("<xmlattr>.conversation", "");
                if (!conversation.empty()) {
                    auto cached = conversations.find(conversation);
                    if (cached != conversations.end()) {
                        stuff.conversation = cached->second;
                    } else if (VSFileSystem::LookForFile(conversation, VSFileSystem::CommFile) <= VSFileSystem::Ok) {
                        stuff.conversation.reset(new FSM(conversation));
                        conversations[conversation] = stuff.conversation;
                    }
                }
                faction->faction.push_back(stuff);
            }
        }

        // Set index of netural, planets and upgrades factions
        if (tmp_name == "neutral") {
            FactionUtil::neutralfac = factions.size();
        } else if (tmp_name == "planets") {
            FactionUtil::planetfac = factions.size();
        } else if (tmp_name == "upgrades") {
            FactionUtil::upgradefac = factions.size();
        }

        factions.push_back(faction);
        contrabandlists.push_back(contraband);   // turned into units by FactionUtil::LoadContrabandLists
    }
}
