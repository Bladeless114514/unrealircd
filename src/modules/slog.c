/*
 *   IRC - Internet Relay Chat, src/modules/monitor.c
 *   (C) 2021 Bram Matthys and The UnrealIRCd Team
 *
 *   See file AUTHORS in IRC package for additional names of
 *   the programmers.
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 1, or (at your option)
 *   any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include "unrealircd.h"

ModuleHeader MOD_HEADER
  = {
	"slog",
	"5.0",
	"S2S logging", 
	"UnrealIRCd Team",
	"unrealircd-5",
    };

/* Forward declarations */
CMD_FUNC(cmd_slog);
void _do_unreal_log_remote_deliver(LogLevel loglevel, char *subsystem, char *event_id, char *msg, char *json_serialized);
int s2s_json_mtag_is_ok(Client *client, char *name, char *value);
int s2s_json_mtag_can_send(Client *target);

MOD_TEST()
{
	MARK_AS_OFFICIAL_MODULE(modinfo);
	EfunctionAddVoid(modinfo->handle, EFUNC_DO_UNREAL_LOG_REMOTE_DELIVER, _do_unreal_log_remote_deliver);
	return MOD_SUCCESS;
}

MOD_INIT()
{	
	MessageTagHandlerInfo mtag;

	MARK_AS_OFFICIAL_MODULE(modinfo);

	CommandAdd(modinfo->handle, "SLOG", cmd_slog, MAXPARA, CMD_SERVER);

	memset(&mtag, 0, sizeof(mtag));
	mtag.name = "s2s/json";
	mtag.is_ok = s2s_json_mtag_is_ok;
	mtag.can_send = s2s_json_mtag_can_send;
	mtag.flags = MTAG_HANDLER_FLAGS_NO_CAP_NEEDED;
	MessageTagHandlerAdd(modinfo->handle, &mtag);

	return MOD_SUCCESS;
}

MOD_LOAD()
{
	return MOD_SUCCESS;
}

MOD_UNLOAD()
{
	return MOD_SUCCESS;
}

CMD_FUNC(cmd_slog)
{
	LogLevel loglevel;
	char *subsystem;
	char *event_id;
	char *msg;
	char *json_serialized = NULL;
	MessageTag *m;

	if ((parc < 4) || BadPtr(parv[4]))
	{
		sendnumeric(client, ERR_NEEDMOREPARAMS, "SLOG");
		return;
	}

	loglevel = log_level_stringtoval(parv[1]);
	if (loglevel == ULOG_INVALID)
		return;
	subsystem = parv[2];
	if (!valid_subsystem(subsystem))
		return;
	event_id = parv[3];
	if (!valid_event_id(event_id))
		return;
	msg = parv[4];

	m = find_mtag(recv_mtags, "s2s/json");
	if (m)
		json_serialized = m->value;

	/* Call our "from remote" logger */
	if (json_serialized)
		do_unreal_log_internal_from_remote(loglevel, subsystem, event_id, msg, json_serialized ? json_serialized : "");
	else
		unreal_log_raw(loglevel, subsystem, event_id, NULL, msg); // WRONG: this may re-broadcast too, so twice, including back to direction!!!

	/* And broadcast to the other servers */
	sendto_server(client, 0, 0, recv_mtags, ":%s SLOG %s %s %s :%s",
	              client->id,
	              parv[1], parv[2], parv[3], parv[4]);
}

void _do_unreal_log_remote_deliver(LogLevel loglevel, char *subsystem, char *event_id, char *msg, char *json_serialized)
{
	MessageTag *mtags = safe_alloc(sizeof(MessageTag));

	safe_strdup(mtags->name, "s2s/json");
	safe_strdup(mtags->value, json_serialized);

	sendto_server(NULL, 0, 0, mtags, ":%s SLOG %s %s %s :%s",
	              me.id,
	              log_level_valtostring(loglevel), subsystem, event_id, msg);

	free_message_tags(mtags);
}

/** This function verifies if the client sending
 * We simply allow from servers without any syntax checking.
 */
int s2s_json_mtag_is_ok(Client *client, char *name, char *value)
{
	if (IsServer(client) || IsMe(client))
		return 1;

	return 0;
}

/** Outgoing filter for this message tag */
int s2s_json_mtag_can_send(Client *target)
{
	if (IsServer(target))
		return 1;
	return 0;
}
