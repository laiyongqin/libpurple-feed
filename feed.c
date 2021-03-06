/**
* @file feed.c
*
* purple
*
* Copyright (C) 2014 moraxy <moraxy@users.noreply.github.com>
* Based on prple-simple by:
* Copyright (C) 2005 Thomas Butter <butter@uni-mannheim.de>
*
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02111-1301  USA
*/

#include <string.h>

#include "accountopt.h"
#include "blist.h"
#include "conversation.h"
#include "debug.h"
#include "notify.h"
#include "prpl.h"
#include "plugin.h"
#include "util.h"
#include "version.h"
#include "xmlnode.h"

#include <mrss.h>
#include <curl/curl.h>

/* From internal.h */
//TODO
#ifdef ENABLE_NLS
#  include <locale.h>
#  include <libintl.h>
#  define _(String) ((const char *)dgettext(PACKAGE, String))
#  ifdef gettext_noop
#    define N_(String) gettext_noop (String)
#  else
#    define N_(String) (String)
#  endif
#else
#  include <locale.h>
#  define N_(String) (String)
#  ifndef _
#    define _(String) ((const char *)String)
#  endif
#  define ngettext(Singular, Plural, Number) ((Number == 1) ? ((const char *)Singular) : ((const char *)Plural))
#  define dngettext(Domain, Singular, Plural, Number) ((Number == 1) ? ((const char *)Singular) : ((const char *)Plural))
#endif

#ifdef __GNUC__
#  define UNUSED(x) UNUSED_ ## x __attribute__((__unused__))
#else
#  define UNUSED(x) UNUSED_ ## x
#endif

#define FEED_STATUS_ERROR	"Error: "
#define FEED_STATUS_ONLINE	"online"
#define FEED_STATUS_OFFLINE	"offline"

#define PRPLFEED		"prplfeed"

#define DEFAULT_REFRESH_TIME	30

/* Blame these two for this:
 * http://stackoverflow.com/questions/4904255/how-can-i-generate-a-list-via-the-c-preprocessor-cpp
 * http://stackoverflow.com/questions/5106280/string-concatenation-using-preprocessor
 * This could be awesome for unit tests thou :D
 */
// TODO Put as much of this as possible in an header file
#define PRPLFEED_RETURN_CODES(F) \
F(200, "OK", dump) \
F(204, "No Content", dump) \
F(300, "Multiple Choices", dump) \
F(301, "Moved Permanently", dump) \
F(303, "See Other", dump) \
F(304, "Not Modified", dump) \
F(305, "Use Proxy", dump) \
F(308, "Permanent Redirect", dump) \
F(400, "Bad Request (bug?)", dump) \
F(401, "Unauthorized", dump) \
F(403, "Forbidden", dump) \
F(404, "Not Found", dump) \
F(405, "Method Not Allowed (bug?)", dump) \
F(406, "Not Acceptable", dump) \
F(408, "Request Time-out (bug?)", dump) \
F(410, "Gone", dump) \
F(411, "Length Required (bug?)", dump) \
F(412, "Precondition Failed	(bug?)", dump) \
F(413, "Request Entity Too Large (bug?)", dump) \
F(414, "Request-URL Too Long (bug?)", dump) \
F(415, "Unsupported Media Type (bug?)", dump) \
F(420, "Policy Not Fulfilled (bug?)", dump) \
F(421, "There are too many connections from your internet address (bug?)", dump) \
F(422, "Unprocessable Entity (bug?)", dump) \
F(423, "Locked", dump) \
F(424, "Failed Dependency", dump) \
F(426, "Upgrade Required (bug?)", dump) \
F(428, "Precondition Required (bug?)", dump) \
F(429, "Too Many Requests (bug?)", dump) \
F(431, "Request Header Fields Too Large (bug?)" , dump)

typedef void (*purplefeed_return_code_callback)(const int, const gchar*, const gchar*);

GHashTable *return_codes = NULL;

/*
 TODO saved for later, don't need all those -Wunused-function warnings right now
#define PRPLFEED_RETURN_CODE_PROTOTYPES(NAME, MESSAGE, FUNCTION) \
            static void purplefeed_return_code_##NAME##_cb(const int id, const gchar* message, const gchar* url_text);

#define PRPLFEED_RETURN_CODE_BODIES(NAME, MESSAGE, FUNCTION) \
            static void purplefeed_return_code_##NAME##_cb(const int id, const gchar* message, const gchar* url_text) \
            { \
                purple_debug_info(PRPLFEED, "-> %i(%i) %s(%s) %s\n", id, NAME, message, MESSAGE, url_text); \
            }

PRPLFEED_RETURN_CODES(PRPLFEED_RETURN_CODE_PROTOTYPES)
PRPLFEED_RETURN_CODES(PRPLFEED_RETURN_CODE_BODIES)
*/

static void purplefeed_return_code_dump_cb(const int id, const gchar* message, const gchar* url_text);
static void purplefeed_return_code_dump_cb(const int id, const gchar* message, const gchar* url_text)
{
    purple_debug_info(PRPLFEED, "purplefeed_return_code_dump_cb: %i %s %s\n", id, message, url_text);
}

// TODO For now: Just dump everything. Later switch to ##NAME## or proper ##FUNCTION## functions
#define PRPLFEED_RETURN_CODE_HASH_INSERTS(NAME, MESSAGE, FUNCTION)	{ \
    g_hash_table_insert(return_codes, GINT_TO_POINTER(NAME), &purplefeed_return_code_##FUNCTION##_cb); \
}


static const char *prplfeed_list_icon(PurpleAccount *a, PurpleBuddy *b)
{
	return "feed";
}

static const char *prplfeed_list_emblems(PurpleBuddy *b)
{
	// TODO come up with something?
	if(strncmp(b->name, "https", strlen("https")) == 0)
		return "secure";
	else
		return NULL;
}

static void prplfeed_check_buddy_for_updates_cb(PurpleUtilFetchUrlData *url_data, gpointer user_data, const gchar *url_text, gsize len, const gchar *error_message)
{
    const char *ret;
    void (*return_code_cb)(const int, const gchar*, const gchar*);
    const PurpleBuddy *buddy = (PurpleBuddy*)user_data;
    int http_version = -1;
    int return_code = 0;

    purple_debug_info(PRPLFEED, "Err: %s, URL: %s, URL_text: %s\n", error_message, buddy->name, url_text);

    /* This is probably prone to some format error or something */
    sscanf(url_text, "HTTP/1.%1i %3i", &http_version, &return_code);

    /* Lookup and call the proper callback for our return code */
    return_code_cb = g_hash_table_lookup(return_codes, GINT_TO_POINTER(return_code));
    if(!return_code_cb)
        purple_debug_error(PRPLFEED, "Couldn't find a return_code_cb for %i(%i Elements in table), using HTTP/1.%i\n",
                return_code, g_hash_table_size(return_codes), http_version);
    else
        (*return_code_cb)(return_code, "(TODO is the description actually necessary?)", url_text);

    /* General date-relevant fields */
    /* Note: This uses the server's timezone and not necessarily the local one */
#define CHECK_AND_SAVE_HEADER(TEXT, VARNAME) { \
    if((ret = purple_strcasestr(url_text, "\n"TEXT":")) != NULL) \
    { \
        char var[128]; \
        memset(var, 0, sizeof(var)); \
        sscanf(ret, "\n"TEXT": %127[^\r\n]", var); \
        purple_debug_info(PRPLFEED, "-> %s %s(%s)\n", TEXT, var, ret); \
        \
        purple_blist_node_set_string(PURPLE_BLIST_NODE(buddy), VARNAME, var); \
    } \
}
    CHECK_AND_SAVE_HEADER("Cache-Control", "cachecontrol")
	CHECK_AND_SAVE_HEADER("Last Modified", "lastmodified")
    CHECK_AND_SAVE_HEADER("Expires", "expires")
    CHECK_AND_SAVE_HEADER("Date", "lastchecked")
#undef CHECK_HEADER

    /*
    TODO based on return code and parsed headers decide what to do/save (if obey_cache_headers==true)
         e.g. 304 -> all fine, 200 -> proceed with update, 401 -> inform user, etc.
    Relevant headers:
    Cache-Control
    Last-Modified (e.g. http://web.jabber.ccc.de/?feed=rss2 always returns 200 OK, but Last-Modified tells the truth)
    Expires
    */
}

static void prplfeed_check_buddy_for_updates(const PurpleBuddy* buddy)
{
    // TODO SSL testing
    // TODO Proxy testing
    // TODO Auth testing

    char *host;
    char *path;
    char *request;
    char *username;
    char *password;
    char *auth;
    char *auth_base64 = NULL;
    const char *locale;
    const char *last_check;

    /* Get the last time we checked this buddy or default to now */
    if((last_check = purple_blist_node_get_string(PURPLE_BLIST_NODE(buddy), "lastcheck")) == NULL)
    {
        const time_t current_time = time(NULL);

        /* Days and months have to be in English*/
        locale = setlocale(LC_TIME, NULL);
        setlocale(LC_TIME, "en_US.utf8");

        /* Format according to RFC rules */
        last_check = purple_utf8_strftime("%a, %d %b %Y %H:%M:%S %Z", gmtime(&current_time));

        /* Switch back to former locale */
        setlocale(LC_TIME, locale);
    }

    purple_url_parse(buddy->name, &host, NULL, &path, &username, &password);

    /* Basic authorization */
    if(strlen(username) > 0)
    {
        auth = g_strdup_printf("%s:%s", username, password);
        auth_base64 = purple_base64_encode((unsigned char*)&auth, strlen(auth));
        // Authorization: Basic %s\r\n
    }

    /* Our crude and short header request */
    request = g_strdup_printf("HEAD /%s HTTP/1.1\r\n"
				 "User-Agent: " PRPLFEED "\r\n"
				 "Host: %s\r\n"
                 "If-Modified-Since: %s\r\n"
                 "Accept-Encoding: deflate\r\n\r\n",
				  path, host, last_check);

    purple_debug_info(PRPLFEED, "request: %s\n", request);

    /* Actually send the request */
    purple_util_fetch_url_request(buddy->name, TRUE, NULL, TRUE, request, TRUE, prplfeed_check_buddy_for_updates_cb, (gpointer)buddy);

    /* Authorization again */
    if(strlen(username) > 0)
    {
        g_free(auth);
        g_free(auth_base64);
    }

    g_free(host);
    g_free(path);
    g_free(request);
    g_free(username);
    g_free(password);
}

static gboolean prplfeed_request_feed_from_buddy(PurpleBuddy *b, mrss_t **mrss, mrss_options_t *options);
static gboolean prplfeed_feed_check(gpointer userdata)
{
	// TODO too many failed attempts => deactivate buddy?

	mrss_t *data = NULL;
	PurpleBuddy *buddy = (PurpleBuddy*)userdata;

	if(!prplfeed_request_feed_from_buddy(buddy, &data, NULL)) // TODO options
	{
		// TODO better error reporting
//		GString *message = g_string_sized_new(1024);
//		g_string_append_printf(message, "Random %i", rand());
		purple_prpl_got_user_status(buddy->account, buddy->name, FEED_STATUS_ERROR, "message", "See debug log"/*g_string_free(message, FALSE)*/, NULL);
	}
	else
	{
		purple_prpl_got_user_status(buddy->account, buddy->name, FEED_STATUS_ONLINE, NULL);
	}

	if(data)
		mrss_free(data);

    prplfeed_check_buddy_for_updates(buddy);

	return TRUE;
}

static void prplfeed_check_buddy(PurpleConnection *gc, PurpleBuddy *buddy, gpointer userdata)
{
	int refresh_time;
	int timer_id;

	purple_debug_info(PRPLFEED, "%s checking %s\n", gc->account->username, buddy->alias ? buddy->alias : buddy->name);

	timer_id = purple_blist_node_get_int(PURPLE_BLIST_NODE(buddy), "timerid");

	if (timer_id > 0) /* Active and running */
	{
		purple_debug_info(PRPLFEED, "%s is already active\n", buddy->alias ? buddy->alias : buddy->name);
	}
	else if (timer_id <= 0)	/* Possibly activated before but still dormant */
	{
		refresh_time = purple_blist_node_get_int(PURPLE_BLIST_NODE(buddy), "refreshtime");

		if(refresh_time <= 0) /* Total newbie */
		{
			/* First try the group node */
			refresh_time = purple_blist_node_get_int(PURPLE_BLIST_NODE(purple_buddy_get_group(buddy)), "refreshtime");

			/* No group setting either, just use the default */
			if(refresh_time <= 0)
				refresh_time = purple_account_get_int(buddy->account, "refreshtime", DEFAULT_REFRESH_TIME);

			purple_debug_info(PRPLFEED, "First time checking %s, setting a %i minutes timer\n", buddy->alias ? buddy->alias : buddy->name, refresh_time);
		}
		else /* Just sleepy */
		{
			purple_debug_info(PRPLFEED, "%s was checked before but is inactive, setting a %i minutes timer\n", buddy->alias ? buddy->alias : buddy->name, refresh_time);
		}

		/* Start timer */
		timer_id = purple_timeout_add_seconds(refresh_time/* TODO don't forget on release: refreshTime*60) */, prplfeed_feed_check, buddy);

		/* Save for future checks */
		purple_blist_node_set_int(PURPLE_BLIST_NODE(buddy), "refreshtime", refresh_time);
		purple_blist_node_set_int(PURPLE_BLIST_NODE(buddy), "timerid", timer_id);

		/* Don't wait for the first interval to finish */
		prplfeed_feed_check((gpointer)buddy);
	}
}

static void prplfeed_login(PurpleAccount *acct)
{
	GSList *buddies;
    guint buddies_count;
    guint n;
	PurpleConnection *gc = purple_account_get_connection(acct);

	purple_debug_info(PRPLFEED, "logging in %s\n", acct->username);

    buddies = purple_find_buddies(acct, NULL);
    buddies_count = g_slist_length(buddies);

	purple_connection_update_progress(gc, _("Loading buddies"), 0, buddies_count + 3);

	for (n = 1; buddies; buddies = g_slist_delete_link(buddies, buddies), n++)
	{
		PurpleBuddy *buddy = buddies->data;

		prplfeed_check_buddy(gc, buddy, NULL);

        purple_connection_update_progress(gc, buddy->alias ? buddy->alias : buddy->name, n, buddies_count + 3);

        purple_debug_info(PRPLFEED, "checked %i (%s)\n", n, buddy->alias ? buddy->alias : buddy->name);
	}

	purple_connection_update_progress(gc, _("Finished loading buddies"), n, buddies_count + 3);

    /* Fill our 'return code' -> 'callback function' table */
    return_codes = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, NULL);
    PRPLFEED_RETURN_CODES(PRPLFEED_RETURN_CODE_HASH_INSERTS)

    purple_connection_update_progress(gc, _("Initialized return codes table"), n + 1, buddies_count + 3);

	purple_debug_info(PRPLFEED, "done logging in %s\n", acct->username);

	purple_connection_set_state(gc, PURPLE_CONNECTED);
}

static void prplfeed_close(PurpleConnection *gc)
{
	// TODO close and deactivate active timers, connections, URL requests...
	GSList *buddies;

	purple_debug_info(PRPLFEED, "closing %s\n", gc->account->username);

	for (buddies = purple_find_buddies(gc->account, NULL); buddies; buddies = g_slist_delete_link(buddies, buddies))
	{
		PurpleBuddy *buddy = buddies->data;

		purple_blist_node_remove_setting(PURPLE_BLIST_NODE(buddy), "timerid");
	}

    g_hash_table_destroy(return_codes);

    purple_debug_info(PRPLFEED, "done closing %s\n", gc->account->username);
}

static void prplfeed_add_buddy(PurpleConnection *gc, PurpleBuddy *buddy, PurpleGroup *group)
{
	purple_debug_info(PRPLFEED,
			"added %s (alias %s) to %s's buddy list in group %s\n",
			buddy->name, buddy->alias ? buddy->alias : "--",
			gc->account->username,
			group ? purple_group_get_name(group) : "(no group)");

	prplfeed_check_buddy(gc, buddy, NULL);

	// TODO set buddy's icon based on feed data/favicon/etc.
}

static gboolean prplfeed_request_feed_from_buddy(PurpleBuddy *b, mrss_t **mrss, mrss_options_t *options)
{
	mrss_error_t ret;
	CURLcode curlcode;

	ret = mrss_parse_url_with_options_and_error(b->name, mrss, options, &curlcode);
	if (ret)
	{
		purple_debug_error(PRPLFEED, "Error in feed %s (ret: %i, curl: %s, mrss: %s)\n", b->name, ret, mrss_curl_strerror(curlcode), mrss_strerror(ret));

		return FALSE;
	}
	
	return TRUE;
}

static void blist_example_menu_item(PurpleBlistNode *node, gpointer userdata)
{
	mrss_t *data = NULL;
	mrss_item_t *item;
	PurpleConversation *convo;

	if(!prplfeed_request_feed_from_buddy((PurpleBuddy*)node, &data, NULL))
		return;
	
	convo = purple_conversation_new(PURPLE_CONV_TYPE_IM, ((PurpleBuddy*)node)->account, "Feed IM");
	purple_conversation_present(convo);
//	purple_conversation_set_features(convo, PURPLE_CONNECTION_HTML); // TODO HTML or not?

	for (item = data->item; item; item = item->next)
	{
		purple_debug_info(PRPLFEED, "%s Item: '%s' pubDate: '%s'\n", data->title, item->title, item->pubDate);

		/*
		purple_conversation_write (PurpleConversation *conv, const char *who,
		const char *message, PurpleMessageFlags flags, time_t mtime)

		Quote: "This function should not be used to write IM or chat
		messages. Use purple_conv_im_write() and purple_conv_chat_write()
		instead. Those functions will most likely call this anyway, but
		they may do their own formatting, sound playback, etc.

		This can be used to write generic messages, such as "so and so
		closed the conversation window.""
		*/
		purple_conv_im_write(PURPLE_CONV_IM(convo),
			data->title, item->description, 
			PURPLE_MESSAGE_RECV, curl_getdate(item->pubDate, NULL));
	}

	if(data)
		mrss_free(data);
}

static GList *prplfeed_blist_node_menu(PurpleBlistNode *node)
{
	if (PURPLE_BLIST_NODE_IS_BUDDY(node))
	{
		PurpleMenuAction *action = purple_menu_action_new(_("Feed URL Test"), PURPLE_CALLBACK(blist_example_menu_item), NULL, NULL);

		return g_list_append(NULL, action);
	}
	else if (PURPLE_BLIST_NODE_IS_GROUP(node))
	{
		// TODO mass-set update interval?
		// problem: a group can have sub-nodes from multiple accounts
		/*
		PurpleMenuAction *action = purple_menu_action_new(
			_("Set group update interval"),
			PURPLE_CALLBACK(prplfeed_set_group_update_interval),
			NULL, NULL);

		return g_list_append(NULL, action);
		*/

		return NULL;
	}
	else
	{
		return NULL;
	}
}

static void prplfeed_input_user_info(PurplePluginAction *action)
{
	PurpleConnection *gc = (PurpleConnection *)action->context;
	PurpleAccount *acct = purple_connection_get_account(gc);
	purple_debug_info(PRPLFEED, "showing 'Set User Info' dialog for %s\n", acct->username);

	purple_account_request_change_user_info(acct);
}

static GList *prplfeed_actions(PurplePlugin *plugin, gpointer context)
{
	// TODO currently pointless, remove or replace with something useful
	PurplePluginAction *action = purple_plugin_action_new(_("Set User Info..."), prplfeed_input_user_info);
	return g_list_append(NULL, action);
}

static char *prplfeed_status_text(PurpleBuddy *buddy)
{
	purple_debug_info(PRPLFEED, "getting %s's status text for %s\n", buddy->alias ? buddy->alias : buddy->name, buddy->account->username);

	if (purple_find_buddy(buddy->account, buddy->name))
	{
		const PurplePresence *presence = purple_buddy_get_presence(buddy);
		const PurpleStatus *status = purple_presence_get_active_status(presence);
		const char *name = purple_status_get_name(status);
		const char *status_id = purple_status_get_id(status);
		const char *message = purple_status_get_attr_string(status, "message");

		char *text;
		if (message && strlen(message) > 0)
			text = g_strdup_printf("%s: %s", name, message);
		else
			text = g_strdup(name);

		purple_debug_info(PRPLFEED, "%s's status text is %s (%s)\n", buddy->alias ? buddy->alias : buddy->name, text, status_id);

		return text;
	}
	else
	{
		//TODO
		purple_debug_info(PRPLFEED, "...but %s does not exist?\n", buddy->alias ? buddy->alias : buddy->name);

		return g_strdup("Missing?");
	}
}

static GHashTable* prplfeed_get_text_table(PurpleAccount* acc)
{
	GHashTable* table;

	table = g_hash_table_new(g_str_hash, g_str_equal);

	g_hash_table_insert(table, "login_label", (gpointer)_("Not really important"));

	return table;
}

static GList *prplfeed_status_types(PurpleAccount *acct)
{
	GList *types = NULL;
	PurpleStatusType *type;

	purple_debug_info(PRPLFEED, "returning status types for %s: %s, %s, %s\n", acct->username, FEED_STATUS_ERROR, FEED_STATUS_ONLINE, FEED_STATUS_OFFLINE);

	/* User & Feed status */
	type = purple_status_type_new_full(PURPLE_STATUS_AVAILABLE, FEED_STATUS_ONLINE, NULL, TRUE, TRUE, FALSE);
	types = g_list_append(types, type);

	type = purple_status_type_new_full(PURPLE_STATUS_OFFLINE, FEED_STATUS_OFFLINE, NULL, TRUE, TRUE, FALSE);
	types = g_list_append(types, type);

	/* Feed-only status */
	type = purple_status_type_new_with_attrs(PURPLE_STATUS_UNAVAILABLE,
			FEED_STATUS_ERROR, FEED_STATUS_ERROR, FALSE, FALSE, FALSE,
			"message", _("Message"), purple_value_new(PURPLE_TYPE_STRING),
			NULL);
	types = g_list_append(types, type);

	return types;
}

static PurplePluginProtocolInfo prpl_info =
{
	OPT_PROTO_CHAT_TOPIC | OPT_PROTO_NO_PASSWORD | OPT_PROTO_IM_IMAGE | OPT_PROTO_USE_POINTSIZE,
	NULL,					/* user_splits */
	NULL,					/* protocol_options */
	{   /* icon_spec, a PurpleBuddyIconSpec */
		"png",                 	    /* format */
		0,                          /* min_width */
		0,                          /* min_height */
		128,                        /* max_width */
		128,                        /* max_height */
		10000,                      /* max_filesize */
		PURPLE_ICON_SCALE_DISPLAY,  /* scale_rules */
	},
	prplfeed_list_icon,		/* list_icon */
	prplfeed_list_emblems,		/* list_emblems */
	prplfeed_status_text,		/* status_text */
	NULL,	//TODO			/* tooltip_text */
	prplfeed_status_types, 		/* away_states */
	prplfeed_blist_node_menu,	/* blist_node_menu */
	NULL,	//TODO			/* chat_info */
	NULL,	//TODO			/* chat_info_defaults */
	prplfeed_login,			/* login */
	prplfeed_close,			/* close */
	NULL, 				/* send_im */
	NULL,				/* set_info */
	NULL, 				/* send_typing */
	NULL,				/* get_info */
	NULL,				/* set_status */
	NULL,				/* set_idle */
	NULL,				/* change_passwd */
	prplfeed_add_buddy,		/* add_buddy */
	NULL,				/* add_buddies */
	NULL, //TODO	/* remove_buddy */
	NULL,				/* remove_buddies */
	NULL,				/* add_permit */
	NULL,				/* add_deny */
	NULL,				/* rem_permit */
	NULL,				/* rem_deny */
	NULL,				/* set_permit_deny */
	NULL,				/* join_chat */
	NULL,				/* reject_chat */
	NULL,				/* get_chat_name */
	NULL,				/* chat_invite */
	NULL,				/* chat_leave */
	NULL,				/* chat_whisper */
	NULL,				/* chat_send */
	NULL,				/* keepalive */
	NULL,				/* register_user */
	NULL,				/* get_cb_info */
	NULL,				/* get_cb_away */
	NULL,				/* alias_buddy */
	NULL,				/* group_buddy */
	NULL,				/* rename_group */
	NULL,				/* buddy_free */
	NULL,				/* convo_closed */
	NULL,				/* normalize */
	NULL,				/* set_buddy_icon */
	NULL,				/* remove_group */
	NULL,				/* get_cb_real_name */
	NULL,				/* set_chat_topic */
	NULL,				/* find_blist_chat */
	NULL,				/* roomlist_get_list */
	NULL,				/* roomlist_cancel */
	NULL,				/* roomlist_expand_category */
	NULL,				/* can_receive_file */
	NULL,				/* send_file */
	NULL,				/* new_xfer */
	NULL,				/* offline_message */
	NULL,				/* whiteboard_prpl_ops */
	NULL,				/* send_raw */
	NULL,				/* roomlist_room_serialize */
	NULL,				/* unregister_user */
	NULL,				/* send_attention */
	NULL,				/* get_attention_types */
	sizeof(PurplePluginProtocolInfo),	/* struct_size */
	prplfeed_get_text_table,	/* get_account_text_table */
	NULL,				/* initiate_media */
	NULL,				/* get_media_caps */
	NULL,				/* get_moods */
	NULL,				/* set_public_alias */
	NULL,				/* get_public_alias */
	NULL,				/* add_buddy_with_invite */
	NULL				/* add_buddies_with_invite */
};

static PurplePluginInfo info =
{
	PURPLE_PLUGIN_MAGIC,
	PURPLE_MAJOR_VERSION,
	PURPLE_MINOR_VERSION,
	PURPLE_PLUGIN_PROTOCOL,                           /**< type           */
	NULL,                                             /**< ui_requirement */
	0,                                                /**< flags          */
	NULL,                                             /**< dependencies   */
	PURPLE_PRIORITY_DEFAULT,                          /**< priority       */
	PRPLFEED,                        	          /**< id             */
	"PurpleFeed",                                     /**< name           */
	/*DISPLAY_VERSION*/ "2.10.9", //TODO                                  /**< version        */
	N_("RSS/Atom Plugin"),                            /**< summary        */
	N_("RSS/Atom FeedReader Plugin"),                 /**< description    */
	"moraxy <moraxy@users.noreply.github.com>",       /**< author         */
	"https://www.github.com/moraxy",                  /**< homepage       */

	NULL,                                             /**< load           */
	NULL,                                             /**< unload         */
	NULL,                                             /**< destroy        */

	NULL,                                             /**< ui_info        */
	&prpl_info,                                       /**< extra_info     */
	NULL,
	prplfeed_actions,

	/* padding */
	NULL,
	NULL,
	NULL,
	NULL
};

static void _init_plugin(PurplePlugin *plugin)
{
	PurpleAccountOption *option;
	GList *presentation_options = NULL;

	purple_debug_info(PRPLFEED, "starting up\n");

	option = purple_account_option_int_new(_("Default refresh time (minutes)"), "refreshtime", DEFAULT_REFRESH_TIME);
	prpl_info.protocol_options = g_list_append(prpl_info.protocol_options, option);

#define ADD_VALUE(list, desc, v) { \
	PurpleKeyValuePair *kvp = g_new0(PurpleKeyValuePair, 1); \
	kvp->key = g_strdup((desc)); \
	kvp->value = g_strdup((v)); \
	list = g_list_prepend(list, kvp); \
	}
	ADD_VALUE(presentation_options, _("Every feed in its own IM window"), "present_single");
	ADD_VALUE(presentation_options, _("Every group of feeds in its own chat window"), "present_group");
	ADD_VALUE(presentation_options, _("Every feed in the same chat window"), "present_all");
#undef ADD_VALUE

	presentation_options = g_list_reverse(presentation_options);

	option = purple_account_option_list_new(_("Default presentation"), "presentation", presentation_options);
	prpl_info.protocol_options = g_list_append(prpl_info.protocol_options, option);

	option = purple_account_option_bool_new(_("IM window instead of chat"), "presentation_im_window", FALSE);
	prpl_info.protocol_options = g_list_append(prpl_info.protocol_options, option);

	option = purple_account_option_bool_new(_("Show status text in buddylist"), "presentation_buddy_status", FALSE);
	prpl_info.protocol_options = g_list_append(prpl_info.protocol_options, option);

	option = purple_account_option_bool_new(_("Follow Cache-Control and Expires headers"), "obey_cache_headers", TRUE);
	prpl_info.protocol_options = g_list_append(prpl_info.protocol_options, option);

	option = purple_account_option_string_new("NOTE", "infomsg", "None of this works yet! :(");
	prpl_info.protocol_options = g_list_append(prpl_info.protocol_options, option);
}

PURPLE_INIT_PLUGIN(prplfeed, _init_plugin, info);
