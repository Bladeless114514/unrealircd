/*
 *   IRC - Internet Relay Chat, src/modules/m_sasl.c
 *   (C) 2012 The UnrealIRCd Team
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

ModuleHeader MOD_HEADER(sasl)
  = {
	"sasl",
	"5.0",
	"SASL", 
	"3.2-b8-1",
	NULL 
    };

/* Forward declarations */
void saslmechlist_free(ModData *m);
char *saslmechlist_serialize(ModData *m);
void saslmechlist_unserialize(char *str, ModData *m);
char *sasl_capability_parameter(aClient *acptr);
int sasl_server_synched(aClient *sptr);

/* Macros */
#define MSG_AUTHENTICATE "AUTHENTICATE"
#define MSG_SASL "SASL"
#define MSG_SVSLOGIN "SVSLOGIN"
#define AGENT_SID(agent_p)	(agent_p->user != NULL ? agent_p->user->server : agent_p->name)

/* Variables */
long CAP_SASL = 0L;

/*
 * This is a "lightweight" SASL implementation/stack which uses psuedo-identifiers
 * to identify connecting clients.  In Unreal 3.3, we should use real identifiers
 * so that SASL sessions can be correlated.
 *
 * The following people were involved in making the current iteration of SASL over
 * IRC which allowed psuedo-identifiers:
 *
 * danieldg, Daniel de Graff <danieldg@inspircd.org>
 * jilles, Jilles Tjoelker <jilles@stack.nl>
 * Jobe, Matthew Beeching <jobe@mdbnet.co.uk>
 * gxti, Michael Tharp <gxti@partiallystapled.com>
 * nenolod, William Pitcock <nenolod@dereferenced.org>
 *
 * Thanks also to all of the client authors which have implemented SASL in their
 * clients.  With the backwards-compatibility layer allowing "lightweight" SASL
 * implementations, we now truly have a universal authentication mechanism for
 * IRC.
 */

/*
 * decode_puid
 *
 * Decode PUID sent from a SASL agent.  If the servername in the PUID doesn't match
 * ours, we reject the PUID (by returning NULL).
 */
static aClient *decode_puid(char *puid)
{
	aClient *cptr;
	char *it, *it2;
	int cookie = 0;

	if ((it = strrchr(puid, '!')) == NULL)
		return NULL;

	*it++ = '\0';

	if ((it2 = strrchr(it, '.')) != NULL)
	{
		*it2++ = '\0';
		cookie = atoi(it2);
	}

	if (stricmp(me.name, puid))
		return NULL;

	list_for_each_entry(cptr, &unknown_list, lclient_node)
		if (cptr->local->sasl_cookie == cookie)
			return cptr;

	return NULL;
}

/*
 * encode_puid
 *
 * Encode PUID based on aClient.
 */
static const char *encode_puid(aClient *client)
{
	static char buf[HOSTLEN + 20];

	if (MyClient(client))
		return client->id;

	/* create a cookie if necessary (and in case getrandom16 returns 0, then run again) */
	while (!client->local->sasl_cookie)
		client->local->sasl_cookie = getrandom16();

	snprintf(buf, sizeof buf, "%s!0.%d", me.name, client->local->sasl_cookie);

	return buf;
}

/*
 * SVSLOGIN message
 *
 * parv[1]: propagation mask
 * parv[2]: target PUID
 * parv[3]: ESVID
 */
CMD_FUNC(m_svslogin)
{
	if (!SASL_SERVER || MyClient(sptr) || (parc < 3) || !parv[3])
		return 0;

	if (!stricmp(parv[1], me.name))
	{
		aClient *target_p;

		target_p = find_client(parv[2], NULL);
		if (target_p && !MyConnect(target_p))
			return 0;

		/* is the PUID valid? */
		if (!target_p && ((target_p = decode_puid(parv[2])) == NULL))
			return 0;

		if (target_p->user == NULL)
			make_user(target_p);

		strlcpy(target_p->user->svid, parv[3], sizeof(target_p->user->svid));

		sendnumeric(target_p, RPL_LOGGEDIN,
			   BadPtr(target_p->name) ? "*" : target_p->name,
			   BadPtr(target_p->user->username) ? "*" : target_p->user->username,
			   BadPtr(target_p->user->realhost) ? "*" : target_p->user->realhost,
			   target_p->user->svid, target_p->user->svid);

		return 0;
	}

	/* not for us; propagate. */
	sendto_server(cptr, 0, 0, NULL, ":%s SVSLOGIN %s %s %s",
	    sptr->name, parv[1], parv[2], parv[3]);

	return 0;
}

/*
 * SASL message
 *
 * parv[1]: distribution mask
 * parv[2]: target PUID
 * parv[3]: mode/state
 * parv[4]: data
 * parv[5]: out-of-bound data
 */
CMD_FUNC(m_sasl)
{
	if (!SASL_SERVER || MyClient(sptr) || (parc < 4) || !parv[4])
		return 0;

	if (!stricmp(parv[1], me.name))
	{
		aClient *target_p;

		target_p = find_client(parv[2], NULL);
		if (target_p && !MyConnect(target_p))
			return 0;

		/* is the PUID valid? */
		if (!target_p && ((target_p = decode_puid(parv[2])) == NULL))
			return 0;

		if (target_p->user == NULL)
			make_user(target_p);

		/* reject if another SASL agent is answering */
		if (*target_p->local->sasl_agent && stricmp(sptr->name, target_p->local->sasl_agent))
			return 0;
		else
			strlcpy(target_p->local->sasl_agent, sptr->name, sizeof(target_p->local->sasl_agent));

		if (*parv[3] == 'C')
		{
			RunHookReturnInt2(HOOKTYPE_SASL_CONTINUATION, target_p, parv[4], !=0);
			sendto_one(target_p, NULL, "AUTHENTICATE %s", parv[4]);
		}
		else if (*parv[3] == 'D')
		{
			*target_p->local->sasl_agent = '\0';
			if (*parv[4] == 'F')
			{
				target_p->local->since += 7; /* bump fakelag due to failed authentication attempt */
				RunHookReturnInt2(HOOKTYPE_SASL_RESULT, target_p, 0, !=0);
				sendnumeric(target_p, ERR_SASLFAIL);
			}
			else if (*parv[4] == 'S')
			{
				target_p->local->sasl_complete++;
				RunHookReturnInt2(HOOKTYPE_SASL_RESULT, target_p, 1, !=0);
				sendnumeric(target_p, RPL_SASLSUCCESS);
			}
		}
		else if (*parv[3] == 'M')
			sendnumeric(target_p, RPL_SASLMECHS, parv[4]);

		return 0;
	}

	/* not for us; propagate. */
	sendto_server(cptr, 0, 0, NULL, ":%s SASL %s %s %c %s %s",
	    sptr->name, parv[1], parv[2], *parv[3], parv[4], parc > 5 ? parv[5] : "");

	return 0;
}

/*
 * AUTHENTICATE message
 *
 * parv[1]: data
 */
CMD_FUNC(m_authenticate)
{
	aClient *agent_p = NULL;

	/* Failing to use CAP REQ for sasl is a protocol violation. */
	if (!SASL_SERVER || !MyConnect(sptr) || BadPtr(parv[1]) || !HasCapability(sptr, "sasl"))
		return 0;

	if ((parv[1][0] == ':') || strchr(parv[1], ' '))
	{
		sendnumeric(sptr, ERR_CANNOTDOCOMMAND, "AUTHENTICATE", "Invalid parameter");
		return 0;
	}

	if (strlen(parv[1]) > 400)
	{
		sendnumeric(sptr, ERR_SASLTOOLONG);
		return 0;
	}

	if (*sptr->local->sasl_agent)
		agent_p = find_client(sptr->local->sasl_agent, NULL);

	if (agent_p == NULL)
	{
		char *addr = BadPtr(sptr->ip) ? "0" : sptr->ip;
		char *certfp = moddata_client_get(sptr, "certfp");

		sendto_server(NULL, 0, 0, NULL, ":%s SASL %s %s H %s %s",
		    me.name, SASL_SERVER, encode_puid(sptr), addr, addr);

		if (certfp)
			sendto_server(NULL, 0, 0, NULL, ":%s SASL %s %s S %s %s",
			    me.name, SASL_SERVER, encode_puid(sptr), parv[1], certfp);
		else
			sendto_server(NULL, 0, 0, NULL, ":%s SASL %s %s S %s",
			    me.name, SASL_SERVER, encode_puid(sptr), parv[1]);
	}
	else
		sendto_server(NULL, 0, 0, NULL, ":%s SASL %s %s C %s",
		    me.name, AGENT_SID(agent_p), encode_puid(sptr), parv[1]);

	sptr->local->sasl_out++;

	return 0;
}

static int abort_sasl(aClient *cptr)
{
	if (cptr->local->sasl_out == 0 || cptr->local->sasl_complete)
		return 0;

	cptr->local->sasl_out = cptr->local->sasl_complete = 0;
	sendnumeric(cptr, ERR_SASLABORTED);

	if (*cptr->local->sasl_agent)
	{
		aClient *agent_p = find_client(cptr->local->sasl_agent, NULL);

		if (agent_p != NULL)
		{
			sendto_server(NULL, 0, 0, NULL, ":%s SASL %s %s D A",
			    me.name, AGENT_SID(agent_p), encode_puid(cptr));
			return 0;
		}
	}

	sendto_server(NULL, 0, 0, NULL, ":%s SASL * %s D A", me.name, encode_puid(cptr));
	return 0;
}

/** Is this capability visible?
 * Note that 'sptr' may be NULL when queried from CAP DEL / CAP NEW
 */
int sasl_capability_visible(aClient *sptr)
{
	if (!SASL_SERVER || !find_server(SASL_SERVER, NULL))
		return 0;

	/* Don't advertise 'sasl' capability if we are going to reject the
	 * user anyway due to set::plaintext-policy. This way the client
	 * won't attempt SASL authentication and thus it prevents the client
	 * from sending the password unencrypted (in case of method PLAIN).
	 */
	if (sptr && !IsSecure(sptr) && !IsLocal(sptr) && (iConf.plaintext_policy_user == POLICY_DENY))
		return 0;

	/* Similarly, don't advertise when we are going to reject the user
	 * due to set::outdated-tls-policy.
	 */
	if (IsSecure(sptr) && (iConf.outdated_tls_policy_user == POLICY_DENY) && outdated_tls_client(sptr))
		return 0;

	return 1;
}

int sasl_connect(aClient *sptr)
{
	return abort_sasl(sptr);
}

int sasl_quit(aClient *sptr, char *comment)
{
	return abort_sasl(sptr);
}

int sasl_server_quit(aClient *sptr)
{
	if (!SASL_SERVER)
		return 0;

	/* If the set::sasl-server is gone, let everyone know 'sasl' is no longer available */
	if (!strcasecmp(sptr->name, SASL_SERVER))
		send_cap_notify(0, "sasl");

	return 0;
}

void auto_discover_sasl_server(int justlinked)
{
	if (!SASL_SERVER && SERVICES_NAME)
	{
		aClient *acptr = find_server(SERVICES_NAME, NULL);
		if (acptr && moddata_client_get(acptr, "saslmechlist"))
		{
			/* SASL server found */
			if (justlinked)
			{
				/* Let's send this message only on link and not also on /rehash */
				sendto_realops("Services server '%s' provides SASL authentication, good! "
				               "I'm setting set::sasl-server to '%s' internally.",
				               SERVICES_NAME, SERVICES_NAME);
				/* We should really get some LOG_INFO or something... I keep abusing LOG_ERROR :) */
				ircd_log(LOG_ERROR, "Services server '%s' provides SASL authentication, good! "
				                    "I'm setting set::sasl-server to '%s' internally.",
				                    SERVICES_NAME, SERVICES_NAME);
			}
			safestrdup(SASL_SERVER, SERVICES_NAME);
			if (justlinked)
				sasl_server_synched(acptr);
		}
	}
}

int sasl_server_synched(aClient *sptr)
{
	if (!SASL_SERVER)
	{
		auto_discover_sasl_server(1);
		return 0;
	}

	/* If the set::sasl-server is gone, let everyone know 'sasl' is no longer available */
	if (!strcasecmp(sptr->name, SASL_SERVER))
		send_cap_notify(1, "sasl");

	return 0;
}

MOD_INIT(sasl)
{
	ClientCapabilityInfo cap;
	ModDataInfo mreq;

	MARK_AS_OFFICIAL_MODULE(modinfo);

	CommandAdd(modinfo->handle, MSG_SASL, m_sasl, MAXPARA, M_USER|M_SERVER);
	CommandAdd(modinfo->handle, MSG_SVSLOGIN, m_svslogin, MAXPARA, M_USER|M_SERVER);
	CommandAdd(modinfo->handle, MSG_AUTHENTICATE, m_authenticate, MAXPARA, M_UNREGISTERED|M_USER);

	HookAdd(modinfo->handle, HOOKTYPE_LOCAL_CONNECT, 0, sasl_connect);
	HookAdd(modinfo->handle, HOOKTYPE_LOCAL_QUIT, 0, sasl_quit);
	HookAdd(modinfo->handle, HOOKTYPE_SERVER_QUIT, 0, sasl_server_quit);
	HookAdd(modinfo->handle, HOOKTYPE_SERVER_SYNCHED, 0, sasl_server_synched);

	memset(&cap, 0, sizeof(cap));
	cap.name = "sasl";
	cap.visible = sasl_capability_visible;
	cap.parameter = sasl_capability_parameter;
	ClientCapabilityAdd(modinfo->handle, &cap, &CAP_SASL);

	memset(&mreq, 0, sizeof(mreq));
	mreq.name = "saslmechlist";
	mreq.free = saslmechlist_free;
	mreq.serialize = saslmechlist_serialize;
	mreq.unserialize = saslmechlist_unserialize;
	mreq.sync = 1;
	mreq.type = MODDATATYPE_CLIENT;
	ModDataAdd(modinfo->handle, mreq);

	return MOD_SUCCESS;
}

MOD_LOAD(sasl)
{
	auto_discover_sasl_server(0);
	return MOD_SUCCESS;
}

MOD_UNLOAD(sasl)
{
	return MOD_SUCCESS;
}

void saslmechlist_free(ModData *m)
{
	if (m->str)
		MyFree(m->str);
}

char *saslmechlist_serialize(ModData *m)
{
	if (!m->str)
		return NULL;
	return m->str;
}

void saslmechlist_unserialize(char *str, ModData *m)
{
	if (m->str)
		MyFree(m->str);
	m->str = strdup(str);
}

char *sasl_capability_parameter(aClient *acptr)
{
	aClient *server;

	if (SASL_SERVER)
	{
		server = find_server(SASL_SERVER, NULL);
		if (server)
			return moddata_client_get(server, "saslmechlist"); /* NOTE: could still return NULL */
	}

	return NULL;
}