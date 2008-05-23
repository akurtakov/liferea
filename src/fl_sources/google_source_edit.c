#include <glib.h>
#include <gtk/gtk.h>
#include <string.h>
#include "debug.h"
#include "update.h"
#include "subscription.h"
#include "common.h"


#include "google_source.h"
#include "google_source_edit.h"
#include "config.h"
#include <libxml/xmlwriter.h>
#include <libxml/xmlreader.h>
#include <glib/gstdio.h>
#include "xml.h"

typedef struct editCtxt { 
	int     readerId ;
	editPtr edit; 
} *editCtxtPtr; 

editPtr 
google_source_edit_new (void)
{
	editPtr edit = g_new0 (struct edit, 1);
	return edit;
}

void 
google_source_edit_free (editPtr edit)
{ 
	g_free (edit->guid);
	g_free (edit->feedUrl);
	g_free (edit);
}

editCtxtPtr
google_source_edit_context_new(readerPtr reader, editPtr edit)
{
	editCtxtPtr ctxt = g_new0(struct editCtxt, 1);
	ctxt->readerId = reader->id;
	ctxt->edit = edit;
	return ctxt ;
}
gchar* google_source_edit_get_cachefile (readerPtr reader) 
{
	return common_create_cache_filename("cache" G_DIR_SEPARATOR_S "plugins", reader->root->id, "edits.xml");
}

static void
google_source_edit_export_helper (editPtr edit, xmlTextWriterPtr writer) 
{
	xmlTextWriterStartElement(writer, BAD_CAST "edit") ;

	gchar* action = g_strdup_printf("%d", edit->action) ;
	xmlTextWriterWriteElement(writer, BAD_CAST "action", action);
	g_free(action);
	if (edit->feedUrl) 
		xmlTextWriterWriteElement(writer, BAD_CAST "feedUrl", edit->feedUrl ) ;
	if (edit->guid) 
		xmlTextWriterWriteElement(writer, BAD_CAST "guid", edit->guid);
	xmlTextWriterEndElement(writer);
}
void
google_source_edit_export (readerPtr reader) 
{ 
	xmlTextWriterPtr writer;
	gchar            *file = google_source_edit_get_cachefile(reader);
	writer = xmlNewTextWriterFilename(file, 0);
	g_free(file) ;
	file = NULL ;
	if ( writer == NULL ) {
		g_warning("Could not create edit cache file\n");
		g_assert(FALSE);
		return ;
	}
	xmlTextWriterStartDocument(writer, NULL, "UTF-8", NULL);

	xmlTextWriterStartElement(writer, BAD_CAST "edits");
	xmlTextWriterWriteAttribute(writer, BAD_CAST "version", 
				    BAD_CAST PACKAGE_VERSION);

	while ( !g_queue_is_empty(reader->editQueue) ) {
		editPtr edit = g_queue_pop_head(reader->editQueue);
		google_source_edit_export_helper(edit, writer);
	}
	
	xmlTextWriterEndElement(writer);
	xmlTextWriterEndDocument(writer);
	xmlFreeTextWriter(writer);
}


void
google_source_edit_import_helper(xmlNodePtr match, gpointer userdata) 
{
	readerPtr reader = (readerPtr) userdata ;
	editPtr edit;
	xmlNodePtr cur; 

	edit = google_source_edit_new() ;
	
	cur = match->children ; 
	while (cur) {
		xmlChar *content = xmlNodeGetContent(cur);
		if ( g_str_equal((gchar*) cur->name, "action")) {
			edit->action = atoi(content) ;
		} else if ( g_str_equal((gchar*) cur->name, "guid") ){ 
			edit->guid = g_strdup((gchar*) content);
		} else if ( g_str_equal((gchar*) cur->name, "feedUrl")) {
			edit->feedUrl = g_strdup((gchar*) content);
		}
		if (content) xmlFree(content);
		cur = cur->next;
	}

	debug3(DEBUG_CACHE, "Found edit request: %d %s %s \n", edit->action, edit->feedUrl, edit->guid);
	google_source_edit_push(reader, edit, FALSE) ;
}
void
google_source_edit_import(readerPtr reader) 
{
	gchar* file = google_source_edit_get_cachefile(reader);

	xmlDocPtr doc = xmlReadFile(file, NULL, 0) ;
	if ( doc == NULL ) {
		g_free(file);
		return ; 
	}

	xmlNodePtr root = xmlDocGetRootElement(doc);
	
	xpath_foreach_match(root, "/edits/edit", google_source_edit_import_helper, 
		reader);

	g_unlink(file); 
	xmlFreeDoc(doc);
	g_free(file); 	
}

static void
google_source_edit_action_complete(
	const struct updateResult* const result, 
	gpointer userdata, 
	updateFlags flags) 
{ 
	editCtxtPtr editCtxt = (editCtxtPtr) userdata ; 
	readerPtr   reader = google_source_reader_from_id(editCtxt->readerId);
	editPtr     edit   = editCtxt->edit ;
	
	g_free(editCtxt);

	if (!reader) {
		google_source_edit_free(edit);
		return ; /* probably got deleted before this callback */
	}
	if ( result->data == NULL || !g_str_equal(result->data, "OK")) {
		if ( edit->callback ) 
			(*edit->callback)(reader, edit, FALSE);
		debug1(DEBUG_UPDATE, "The edit action failed with result: %s\n",
		       result->data);
		google_source_edit_free(edit);
		return ; /** @todo start a timer for next processing */
	}
	
	if ( edit->callback )
		edit->callback(reader, edit, TRUE);

	google_source_edit_free(edit) ;

	/* process anything else waiting on the edit queue */
	google_source_edit_process (reader);
}

static void
google_source_api_add_subscription(editPtr edit, updateRequestPtr request, const gchar* token) 
{
	update_request_set_source(request, "http://www.google.com/reader/api/0/subscription/quickadd?client=liferea");
	gchar* s_escaped = g_uri_escape_string(edit->feedUrl, NULL, TRUE) ;
	gchar* postdata = g_strdup_printf("quickadd=%s&ac=subscribe&T=%s",
					  s_escaped, token) ;
	g_free(s_escaped);
	
	request->postdata = postdata ;
}

static void
google_source_api_remove_subscription(editPtr edit, updateRequestPtr request, const gchar* token) 
{
	update_request_set_source(request, "http://www.google.com/reader/api/0/subscription/edit?client=liferea");
	gchar* s_escaped = g_uri_escape_string(edit->feedUrl, NULL, TRUE);
	g_assert(!request->postdata);
	request->postdata = g_strdup_printf("s=feed%%2F%s&i=null&ac=unsubscribe&T=%s",s_escaped, token);
	g_free(s_escaped);
}
static void 
google_source_api_edit_tag(editPtr edit, updateRequestPtr request, const gchar*token) 
{
	update_request_set_source(request, "http://www.google.com/reader/api/0/edit-tag?client=liferea"); 

	gchar* s_escaped = g_uri_escape_string (edit->feedUrl, NULL, TRUE);
	gchar* a_escaped = NULL ;
	gchar* i_escaped = g_uri_escape_string (edit->guid, NULL, TRUE);
	gchar* postdata = NULL ;

	if (edit->action == EDIT_ACTION_MARK_UNREAD) {
		a_escaped = g_uri_escape_string ("user/-/state/com.google/kept-unread", NULL, TRUE);
		gchar *r_escaped = g_uri_escape_string ("user/-/state/com.google/read", NULL, TRUE);
		postdata = g_strdup_printf ("i=%s&s=feed%%2F%s&a=%s&r=%s&ac=edit-tags&T=%s", i_escaped, s_escaped, a_escaped, r_escaped, token);
		g_free (r_escaped);
	}
	else if (edit->action == EDIT_ACTION_MARK_READ) { 
		a_escaped = g_uri_escape_string ("user/-/state/com.google/read", NULL, TRUE);
		postdata = g_strdup_printf ("i=%s&s=feed%%2F%s&a=%s&ac=edit-tags&T=%s", i_escaped, s_escaped, a_escaped, token);
	}
	else if (edit->action == EDIT_ACTION_TRACKING_MARK_UNREAD) {
		a_escaped = g_uri_escape_string ("user/-/state/com.google/tracking-kept-unread", NULL, TRUE);
		postdata = g_strdup_printf ("i=%s&s=feed%%2F%s&a=%s&ac=edit-tags&async=true&T=%s", i_escaped, s_escaped, a_escaped, token);
	}  else g_assert(FALSE);
	
	g_free (s_escaped);
	g_free (a_escaped); 
	g_free (i_escaped);
	
	debug1 (DEBUG_UPDATE, "google_source: postdata [%s]", postdata);


	request->postdata = postdata;
}

/**
 * Callback from an token request, and sends the actual edit request
 * in processing
 */
static void
google_source_edit_token_cb (const struct updateResult * const result, gpointer userdata, updateFlags flags)
{ 
	if (result->returncode != 0) { 
		/* What is the behaviour that should go here? */
		return;
	}

	readerPtr reader = google_source_reader_from_id(GPOINTER_TO_INT(userdata)); 
	const gchar* token = result->data; 

	if (!reader || g_queue_is_empty (reader->editQueue))
		return;

	editPtr edit = g_queue_peek_head (reader->editQueue);
	updateRequestPtr request = update_request_new ();
	request->updateState = update_state_copy (reader->root->subscription->updateState);
	request->options = update_options_copy (reader->root->subscription->updateOptions) ;
	update_state_set_cookies (request->updateState, reader->sid);

	if ( edit->action == EDIT_ACTION_MARK_READ || 
	     edit->action == EDIT_ACTION_MARK_UNREAD || 
	     edit->action == EDIT_ACTION_TRACKING_MARK_UNREAD ) 
		google_source_api_edit_tag (edit, request, token);
	else if (edit->action == EDIT_ACTION_ADD_SUBSCRIPTION ) 
		google_source_api_add_subscription(edit, request, token);
	else if (edit->action == EDIT_ACTION_REMOVE_SUBSCRIPTION )
		google_source_api_remove_subscription(edit, request, token) ;

	update_execute_request (reader, request, google_source_edit_action_complete, 
	                        google_source_edit_context_new(reader, edit), 0);

	edit = g_queue_pop_head (reader->editQueue);
}

void
google_source_edit_process (readerPtr reader)
{ 
	g_assert (reader);
	if (g_queue_is_empty (reader->editQueue))
		return;
	
	/*
 	* Google reader has a system of tokens. So first, I need to request a 
 	* token from google, before I can make the actual edit request. The
 	* code here is the token code, the actual edit commands are in 
 	* google_source_edit_token_cb
	 */
	updateRequestPtr request = update_request_new ();
	request->updateState = update_state_copy (reader->root->subscription->updateState);
	request->options = update_options_copy (reader->root->subscription->updateOptions);
	request->source = g_strdup ("http://www.google.com/reader/api/0/token");
	update_state_set_cookies (request->updateState, reader->sid);

	update_execute_request (reader, request, google_source_edit_token_cb, 
	                        (gpointer) (reader->id), 0);
}

void
google_source_edit_push_ (readerPtr reader, editPtr edit, gboolean head)
{ 
	g_assert (reader->editQueue);
	if (head) g_queue_push_head (reader->editQueue, edit) ;
	else      g_queue_push_tail (reader->editQueue, edit) ;
}

void 
google_source_edit_push (readerPtr reader, editPtr edit, gboolean head)
{
	g_assert(reader);
	nodePtr root = reader->root ;

	/** @todo any flags I should specify? */
	if (reader->loginState == READER_STATE_NONE) {
		subscription_update(root->subscription, GOOGLE_SOURCE_UPDATE_ONLY_LOGIN) ;
		google_source_edit_push_ (reader, edit, head);
	} else if (reader->loginState == READER_STATE_IN_PROGRESS) {
		google_source_edit_push_ (reader, edit, head);
	} else { 
		google_source_edit_push_ (reader, edit, head);
		google_source_edit_process (reader);
	}
	
}

void
google_source_edit_mark_read (
	readerPtr reader, 
	const gchar *guid,
	const gchar *feedUrl,
	gboolean newStatus)
{
	editPtr edit = google_source_edit_new ();

	edit->guid = g_strdup (guid);
	edit->feedUrl = g_strdup (feedUrl);
	edit->action = newStatus ? EDIT_ACTION_MARK_READ :
	                           EDIT_ACTION_MARK_UNREAD;

	google_source_edit_push (reader, edit, FALSE);

	if (newStatus == FALSE) { 
		/*
		 * According to the Google Reader API, to mark an item unread, 
		 * I also need to mark it as tracking-kept-unread in a separate
		 * network call.
		 */
		edit = google_source_edit_new ();
		edit->guid = g_strdup (guid);
		edit->feedUrl = g_strdup (feedUrl);
		edit->action = EDIT_ACTION_TRACKING_MARK_UNREAD;
		google_source_edit_push (reader, edit, FALSE);
	}
}


void update_subscription_list_callback(readerPtr reader, editPtr edit, gboolean success) 
{
	if ( success ) { 

		/*
		 * It is possible that Google changed the name of the URL that
		 * was sent to it. In that case, I need to recover the URL 
		 * from the list. But a node with the old URL has already 
		 * been created. Allow the subscription update call to fix that.
		 */
		GSList* cur = reader->root->children ;
		for( ; cur ; cur = g_slist_next(cur))  {
			nodePtr node = (nodePtr) cur->data ; 
			if ( g_str_equal(node->subscription->source, edit->feedUrl) ) {
				subscription_set_source(node->subscription, "");
			}
		}
		
		debug0(DEBUG_UPDATE, "Add subscription was successful\n");
		subscription_update(reader->root->subscription, GOOGLE_SOURCE_UPDATE_ONLY_LIST);
	} else 
		debug0(DEBUG_UPDATE, "Failed to add subscription\n");
}
void google_source_edit_add_subscription(
	readerPtr reader, 
	const gchar* feedUrl)
{
	editPtr edit = google_source_edit_new() ;
	edit->action = EDIT_ACTION_ADD_SUBSCRIPTION ; 
	edit->feedUrl = g_strdup(feedUrl) ;
	edit->callback = update_subscription_list_callback ;
	google_source_edit_push(reader, edit, TRUE);
}


void google_source_edit_remove_subscription(readerPtr reader, const gchar* feedUrl) 
{
	editPtr edit = google_source_edit_new(); 
	edit->action = EDIT_ACTION_REMOVE_SUBSCRIPTION ;
	edit->feedUrl = g_strdup(feedUrl) ;
	google_source_edit_push(reader, edit, TRUE) ;
}

gboolean google_source_edit_is_in_queue(readerPtr reader, const gchar* guid) 
{
	/* this is inefficient, but works for the timebeing */
	GList *cur = reader->editQueue->head ; 
	for(; cur; cur = g_list_next(cur)) { 
		editPtr edit = cur->data ; 
		if (edit->guid && g_str_equal(edit->guid, guid))
			return TRUE;
	}
	return FALSE;
}