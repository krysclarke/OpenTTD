/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/** @file script_league.cpp Implementation of ScriptLeagueTable. */

#include "../../stdafx.h"

#include "script_league.hpp"

#include "../script_instance.hpp"
#include "script_error.hpp"
#include "../../league_base.h"
#include "../../league_cmd.h"

#include "../../safeguards.h"


/* static */ bool ScriptLeagueTable::IsValidLeagueTable(LeagueTableID table_id)
{
	return ::LeagueTable::IsValidID(table_id);
}

/* static */ LeagueTableID ScriptLeagueTable::New(Text *title, Text *header, Text *footer)
{
	ScriptObjectRef title_counter(title);
	ScriptObjectRef header_counter(header);
	ScriptObjectRef footer_counter(footer);

	EnforceDeityMode(LEAGUE_TABLE_INVALID);
	EnforcePrecondition(LEAGUE_TABLE_INVALID, title != nullptr);
	EncodedString encoded_title = title->GetEncodedText();
	EnforcePreconditionEncodedText(LEAGUE_TABLE_INVALID, encoded_title);

	EncodedString encoded_header = (header != nullptr ? header->GetEncodedText() : EncodedString{});
	EncodedString encoded_footer = (footer != nullptr ? footer->GetEncodedText() : EncodedString{});

	if (!ScriptObject::Command<CMD_CREATE_LEAGUE_TABLE>::Do(&ScriptInstance::DoCommandReturnLeagueTableID, encoded_title, encoded_header, encoded_footer)) return LEAGUE_TABLE_INVALID;

	/* In case of test-mode, we return LeagueTableID 0 */
	return LeagueTableID::Begin();
}

/* static */ bool ScriptLeagueTable::IsValidLeagueTableElement(LeagueTableElementID element_id)
{
	return ::LeagueTableElement::IsValidID(element_id);
}

/* static */ LeagueTableElementID ScriptLeagueTable::NewElement(LeagueTableID table, SQInteger rating, ScriptCompany::CompanyID company, Text *text, Text *score, LinkType link_type, SQInteger link_target)
{
	ScriptObjectRef text_counter(text);
	ScriptObjectRef score_counter(score);

	EnforceDeityMode(LEAGUE_TABLE_ELEMENT_INVALID);

	EnforcePrecondition(LEAGUE_TABLE_ELEMENT_INVALID, IsValidLeagueTable(table));

	EnforcePrecondition(LEAGUE_TABLE_ELEMENT_INVALID, company == ScriptCompany::COMPANY_INVALID || ScriptCompany::ResolveCompanyID(company) != ScriptCompany::COMPANY_INVALID);
	::CompanyID c = ScriptCompany::FromScriptCompanyID(company);

	EnforcePrecondition(LEAGUE_TABLE_ELEMENT_INVALID, text != nullptr);
	EncodedString encoded_text = text->GetEncodedText();
	EnforcePreconditionEncodedText(LEAGUE_TABLE_ELEMENT_INVALID, encoded_text);

	EnforcePrecondition(LEAGUE_TABLE_ELEMENT_INVALID, score != nullptr);
	EncodedString encoded_score = score->GetEncodedText();
	EnforcePreconditionEncodedText(LEAGUE_TABLE_ELEMENT_INVALID, encoded_score);

	EnforcePrecondition(LEAGUE_TABLE_ELEMENT_INVALID, IsValidLink(Link((::LinkType)link_type, link_target)));

	if (!ScriptObject::Command<CMD_CREATE_LEAGUE_TABLE_ELEMENT>::Do(&ScriptInstance::DoCommandReturnLeagueTableElementID, table, rating, c, encoded_text, encoded_score, (::LinkType)link_type, (::LinkTargetID)link_target)) return LEAGUE_TABLE_ELEMENT_INVALID;

	/* In case of test-mode, we return LeagueTableElementID 0 */
	return LeagueTableElementID::Begin();
}

/* static */ bool ScriptLeagueTable::UpdateElementData(LeagueTableElementID element, ScriptCompany::CompanyID company, Text *text, LinkType link_type, SQInteger link_target)
{
	ScriptObjectRef text_counter(text);

	EnforceDeityMode(false);
	EnforcePrecondition(false, IsValidLeagueTableElement(element));

	EnforcePrecondition(false, company == ScriptCompany::COMPANY_INVALID || ScriptCompany::ResolveCompanyID(company) != ScriptCompany::COMPANY_INVALID);
	::CompanyID c = ScriptCompany::FromScriptCompanyID(company);

	EnforcePrecondition(false, text != nullptr);
	EncodedString encoded_text = text->GetEncodedText();
	EnforcePreconditionEncodedText(false, encoded_text);

	EnforcePrecondition(false, IsValidLink(Link((::LinkType)link_type, link_target)));

	return ScriptObject::Command<CMD_UPDATE_LEAGUE_TABLE_ELEMENT_DATA>::Do(element, c, encoded_text, (::LinkType)link_type, (::LinkTargetID)link_target);
}

/* static */ bool ScriptLeagueTable::UpdateElementScore(LeagueTableElementID element, SQInteger rating, Text *score)
{
	ScriptObjectRef score_counter(score);

	EnforceDeityMode(false);
	EnforcePrecondition(false, IsValidLeagueTableElement(element));

	EnforcePrecondition(false, score != nullptr);
	EncodedString encoded_score = score->GetEncodedText();
	EnforcePreconditionEncodedText(false, encoded_score);

	return ScriptObject::Command<CMD_UPDATE_LEAGUE_TABLE_ELEMENT_SCORE>::Do(element, rating, encoded_score);
}

/* static */ bool ScriptLeagueTable::RemoveElement(LeagueTableElementID element)
{
	EnforceDeityMode(false);
	EnforcePrecondition(false, IsValidLeagueTableElement(element));

	return ScriptObject::Command<CMD_REMOVE_LEAGUE_TABLE_ELEMENT>::Do(element);
}
