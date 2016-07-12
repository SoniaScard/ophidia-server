/*
    Ophidia Server
    Copyright (C) 2012-2016 CMCC Foundation

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "oph.nsmap"

#include "oph_known_operators.h"
#include "oph_workflow_engine.h"
#include "oph_rmanager.h"
#include "oph_task_parser_library.h"

#include <unistd.h>
#if defined(_POSIX_THREADS) || defined(_SC_THREADS)
#include <threads.h>
#include <pthread.h>
#endif
#include <signal.h>
#include <mysql.h>

#if defined(_POSIX_THREADS) || defined(_SC_THREADS)
pthread_mutex_t global_flag;
pthread_mutex_t libssh2_flag;
pthread_cond_t termination_flag;
#endif

char* oph_server_location=0;
HASHTBL *oph_server_params=0;
char* oph_server_protocol=0;
char* oph_server_host=0;
char* oph_server_port=0;
int oph_server_timeout=OPH_SERVER_TIMEOUT;
int oph_server_inactivity_timeout=OPH_SERVER_INACTIVITY_TIMEOUT;
int oph_server_workflow_timeout=OPH_SERVER_WORKFLOW_TIMEOUT;
FILE* logfile=0;
char* oph_log_file_name=0;
char* oph_server_cert=0;
char* oph_server_ca=0;
char* oph_server_password=0;
char* oph_rmanager_conf_file=0;
char* oph_auth_location=0;
char* oph_json_location=0;
char* oph_txt_location=0;
char* oph_web_server=0;
char* oph_web_server_location=0;
char* oph_operator_client=0;
char* oph_ip_target_host=0;
char* oph_subm_user=0;
char* oph_subm_user_publk=0;
char* oph_subm_user_privk=0;
char* oph_xml_operators=0;
char* oph_xml_operator_dir=0;
char* oph_user_notifier=0;
unsigned int oph_server_farm_size=0;
unsigned int oph_server_queue_size=0;
oph_rmanager* orm=0;
int oph_service_status=1;
oph_auth_user_bl* bl_head=0;
ophidiadb *ophDB=0;

void set_global_values(const char* configuration_file)
{
	if (!configuration_file) return;
	pmesg(LOG_INFO, __FILE__,__LINE__,"Loading configuration from '%s'\n",configuration_file);

	oph_server_params = hashtbl_create(HASHTBL_KEY_NUMBER, NULL);
	if (!oph_server_params) return;

	char tmp[OPH_MAX_STRING_SIZE];
	char* value;
	FILE* file = fopen(configuration_file,"r");
	if (file)
	{
		char key[OPH_MAX_STRING_SIZE], value2[OPH_MAX_STRING_SIZE];
		while (fgets(tmp,OPH_MAX_STRING_SIZE,file))
		{
			if (strlen(tmp))
			{
				tmp[strlen(tmp)-1]='\0';
				if (tmp[0]==OPH_COMMENT_MARK) continue; // Skip possible commented lines
				value=strchr(tmp,OPH_SEPARATOR_KV[0]);
				if (value)
				{
					value++;
					snprintf(key,value-tmp,"%s",tmp);
					if (value[0])
					{
						if (value[0]==OPH_SUBSTITUTION_MARK && !strncasecmp(value+1,OPH_SERVER_LOCATION_STR,strlen(OPH_SERVER_LOCATION_STR)))
						{
							snprintf(value2,OPH_MAX_STRING_SIZE,"%s%s",oph_server_location,value+strlen(OPH_SERVER_LOCATION_STR)+1);
							value = value2;
						}
						hashtbl_insert(oph_server_params, key, value);
					}
					else hashtbl_insert(oph_server_params, key, "");
					pmesg(LOG_DEBUG, __FILE__,__LINE__,"Read %s=%s\n",key,value);
				}
			}
		}
		fclose(file);
	}

	// Pre-process
	if ((value=hashtbl_get(oph_server_params, OPH_SERVER_CONF_TIMEOUT))) oph_server_timeout=strtol(value,NULL,10);
	if ((value=hashtbl_get(oph_server_params, OPH_SERVER_CONF_INACTIVITY_TIMEOUT))) oph_server_inactivity_timeout=strtol(value,NULL,10);
	if ((value=hashtbl_get(oph_server_params, OPH_SERVER_CONF_WORKFLOW_TIMEOUT))) oph_server_workflow_timeout=strtol(value,NULL,10);
	if ((value=hashtbl_get(oph_server_params, OPH_SERVER_CONF_SERVER_FARM_SIZE))) oph_server_farm_size=(unsigned int)strtol(value,NULL,10);
	if ((value=hashtbl_get(oph_server_params, OPH_SERVER_CONF_QUEUE_SIZE))) oph_server_queue_size=(unsigned int)strtol(value,NULL,10);
	if (!logfile && (value=hashtbl_get(oph_server_params, OPH_SERVER_CONF_LOGFILE)))
	{
		pmesg(LOG_INFO, __FILE__,__LINE__,"Selected log file '%s'\n",value);
		logfile = fopen(value,"a");
		if (logfile) set_log_file(logfile);

		// Redirect stdout and stderr to logfile
		if (!freopen(value,"a",stdout)) pmesg(LOG_ERROR, __FILE__,__LINE__,"Error in redirect stdout to logfile\n");
		if (!freopen(value,"a",stderr)) pmesg(LOG_ERROR, __FILE__,__LINE__,"Error in redirect stderr to logfile\n");
	}

	// Default values
	if (!oph_server_protocol && !(oph_server_protocol = hashtbl_get(oph_server_params, OPH_SERVER_CONF_PROTOCOL)))
	{
		hashtbl_insert(oph_server_params, OPH_SERVER_CONF_PROTOCOL, OPH_DEFAULT_PROTOCOL);
		oph_server_protocol = hashtbl_get(oph_server_params, OPH_SERVER_CONF_PROTOCOL);
	}
	if (!oph_server_host && !(oph_server_host = hashtbl_get(oph_server_params, OPH_SERVER_CONF_HOST)))
	{
		if (!gethostname(tmp,OPH_MAX_STRING_SIZE)) hashtbl_insert(oph_server_params, OPH_SERVER_CONF_HOST, tmp);
		else hashtbl_insert(oph_server_params, OPH_SERVER_CONF_HOST, OPH_DEFAULT_HOST);
		oph_server_host = hashtbl_get(oph_server_params, OPH_SERVER_CONF_HOST);
	}
	if (!oph_server_port && !(oph_server_port = hashtbl_get(oph_server_params, OPH_SERVER_CONF_PORT)))
	{
		hashtbl_insert(oph_server_params, OPH_SERVER_CONF_PORT, OPH_DEFAULT_PORT);
		oph_server_port = hashtbl_get(oph_server_params, OPH_SERVER_CONF_PORT);
	}
	if (!(oph_server_cert = hashtbl_get(oph_server_params, OPH_SERVER_CONF_CERT)))
	{
		snprintf(tmp,OPH_MAX_STRING_SIZE,OPH_SERVER_CERT,oph_server_location);
		hashtbl_insert(oph_server_params, OPH_SERVER_CONF_CERT, tmp);
		oph_server_cert = hashtbl_get(oph_server_params, OPH_SERVER_CONF_CERT);
	}
	if (!(oph_server_ca = hashtbl_get(oph_server_params, OPH_SERVER_CONF_CA)))
	{
		snprintf(tmp,OPH_MAX_STRING_SIZE,OPH_SERVER_CA,oph_server_location);
		hashtbl_insert(oph_server_params, OPH_SERVER_CONF_CA, tmp);
		oph_server_ca = hashtbl_get(oph_server_params, OPH_SERVER_CONF_CA);
	}
	if (!(oph_server_password = hashtbl_get(oph_server_params, OPH_SERVER_CONF_CERT_PASSWORD)))
	{
		hashtbl_insert(oph_server_params, OPH_SERVER_CONF_CERT_PASSWORD, OPH_SERVER_PASSWORD);
		oph_server_password = hashtbl_get(oph_server_params, OPH_SERVER_CONF_CERT_PASSWORD);
	}
	if (!(oph_rmanager_conf_file = hashtbl_get(oph_server_params, OPH_SERVER_CONF_RMANAGER_CONF_FILE)))
	{
		snprintf(tmp,OPH_MAX_STRING_SIZE,OPH_RMANAGER_CONF_FILE,oph_server_location);
		hashtbl_insert(oph_server_params, OPH_SERVER_CONF_RMANAGER_CONF_FILE, tmp);
		oph_rmanager_conf_file = hashtbl_get(oph_server_params, OPH_SERVER_CONF_RMANAGER_CONF_FILE);
	}
	if (!(oph_auth_location = hashtbl_get(oph_server_params, OPH_SERVER_CONF_AUTHZ_DIR)))
	{
		snprintf(tmp,OPH_MAX_STRING_SIZE,OPH_SERVER_AUTHZ,oph_server_location);
		hashtbl_insert(oph_server_params, OPH_SERVER_CONF_AUTHZ_DIR, tmp);
		oph_auth_location = hashtbl_get(oph_server_params, OPH_SERVER_CONF_AUTHZ_DIR);
	}
	if (!(oph_txt_location = hashtbl_get(oph_server_params, OPH_SERVER_CONF_TXT_DIR)))
	{
		snprintf(tmp,OPH_MAX_STRING_SIZE,OPH_TXT_LOCATION,oph_server_location);
		hashtbl_insert(oph_server_params, OPH_SERVER_CONF_TXT_DIR, tmp);
		oph_txt_location = hashtbl_get(oph_server_params, OPH_SERVER_CONF_TXT_DIR);
	}
	if (!(oph_web_server = hashtbl_get(oph_server_params, OPH_SERVER_CONF_WEB_SERVER)))
	{
		snprintf(tmp,OPH_MAX_STRING_SIZE,OPH_WEB_SERVER);
		hashtbl_insert(oph_server_params, OPH_SERVER_CONF_WEB_SERVER, tmp);
		oph_web_server = hashtbl_get(oph_server_params, OPH_SERVER_CONF_WEB_SERVER);
	}
	if (!(oph_web_server_location = hashtbl_get(oph_server_params, OPH_SERVER_CONF_WEB_SERVER_LOCATION)))
	{
		snprintf(tmp,OPH_MAX_STRING_SIZE,OPH_WEB_SERVER_LOCATION);
		hashtbl_insert(oph_server_params, OPH_SERVER_CONF_WEB_SERVER_LOCATION, tmp);
		oph_web_server_location = hashtbl_get(oph_server_params, OPH_SERVER_CONF_WEB_SERVER_LOCATION);
	}
	if (!(oph_operator_client = hashtbl_get(oph_server_params, OPH_SERVER_CONF_OPERATOR_CLIENT)))
	{
		snprintf(tmp,OPH_MAX_STRING_SIZE,OPH_OPERATOR_CLIENT);
		hashtbl_insert(oph_server_params, OPH_SERVER_CONF_OPERATOR_CLIENT, tmp);
		oph_operator_client = hashtbl_get(oph_server_params, OPH_SERVER_CONF_OPERATOR_CLIENT);
	}
	if (!(oph_ip_target_host = hashtbl_get(oph_server_params, OPH_SERVER_CONF_IP_TARGET_HOST)))
	{
		snprintf(tmp,OPH_MAX_STRING_SIZE,OPH_IP_TARGET_HOST);
		hashtbl_insert(oph_server_params, OPH_SERVER_CONF_IP_TARGET_HOST, tmp);
		oph_ip_target_host = hashtbl_get(oph_server_params, OPH_SERVER_CONF_IP_TARGET_HOST);
	}
	if (!(oph_subm_user = hashtbl_get(oph_server_params, OPH_SERVER_CONF_SUBM_USER)))
	{
		snprintf(tmp,OPH_MAX_STRING_SIZE,OPH_SUBM_USER);
		hashtbl_insert(oph_server_params, OPH_SERVER_CONF_SUBM_USER, tmp);
		oph_subm_user = hashtbl_get(oph_server_params, OPH_SERVER_CONF_SUBM_USER);
	}
	if (!(oph_subm_user_publk = hashtbl_get(oph_server_params, OPH_SERVER_CONF_SUBM_USER_PUBLK)))
	{
		snprintf(tmp,OPH_MAX_STRING_SIZE,OPH_SUBM_USER_PUBLK);
		hashtbl_insert(oph_server_params, OPH_SERVER_CONF_SUBM_USER_PUBLK, tmp);
		oph_subm_user_publk = hashtbl_get(oph_server_params, OPH_SERVER_CONF_SUBM_USER_PUBLK);
	}
	if (!(oph_subm_user_privk = hashtbl_get(oph_server_params, OPH_SERVER_CONF_SUBM_USER_PRIVK)))
	{
		snprintf(tmp,OPH_MAX_STRING_SIZE,OPH_SUBM_USER_PRIVK);
		hashtbl_insert(oph_server_params, OPH_SERVER_CONF_SUBM_USER_PRIVK, tmp);
		oph_subm_user_privk = hashtbl_get(oph_server_params, OPH_SERVER_CONF_SUBM_USER_PRIVK);
	}
	if (!(oph_xml_operators = hashtbl_get(oph_server_params, OPH_SERVER_CONF_XML_URL)))
	{
		snprintf(tmp,OPH_MAX_STRING_SIZE,OPH_CLIENT_XML_URL);
		hashtbl_insert(oph_server_params, OPH_SERVER_CONF_XML_URL, tmp);
		oph_xml_operators = hashtbl_get(oph_server_params, OPH_SERVER_CONF_XML_URL);
	}
	if (!(oph_xml_operator_dir = hashtbl_get(oph_server_params, OPH_SERVER_CONF_XML_DIR)))
	{
		snprintf(tmp,OPH_MAX_STRING_SIZE,OPH_SERVER_XML_EXT_PATH);
		hashtbl_insert(oph_server_params, OPH_SERVER_CONF_XML_DIR, tmp);
		oph_xml_operator_dir = hashtbl_get(oph_server_params, OPH_SERVER_CONF_XML_DIR);
	}
	if (!(oph_user_notifier = hashtbl_get(oph_server_params, OPH_SERVER_CONF_NOTIFIER)))
	{
		snprintf(tmp,OPH_MAX_STRING_SIZE,OPH_USER_NOTIFIER);
		hashtbl_insert(oph_server_params, OPH_SERVER_CONF_NOTIFIER, tmp);
		oph_user_notifier = hashtbl_get(oph_server_params, OPH_SERVER_CONF_NOTIFIER);
	}

	oph_json_location = oph_web_server_location; // Position of JSON Response will be the same of web server
}

void cleanup(){
  if (oph_server_params) hashtbl_destroy(oph_server_params);
#ifdef OPH_SERVER_LOCATION
  if (oph_server_location) free(oph_server_location);
#endif
  if (orm) free_oph_rmanager(orm);
  if (ophDB) oph_odb_free_ophidiadb(ophDB);
#if defined(_POSIX_THREADS) || defined(_SC_THREADS)
  pthread_mutex_destroy(&global_flag);
  pthread_mutex_destroy(&libssh2_flag);
  pthread_cond_destroy(&termination_flag);
#endif
  oph_tp_end_xml_parser();
}

int _check_oph_server(const char* operator, int option)
{
	char sessionid[OPH_MAX_STRING_SIZE];
	snprintf(sessionid,OPH_MAX_STRING_SIZE,"%s/sessions/123/xperiment",oph_web_server);
	
	// Workflow
	oph_workflow* wf = (oph_workflow*)calloc(1,sizeof(oph_workflow));
	if (!wf) return 1;

	// HEADER
	wf->idjob = 1;
	wf->workflowid = 1;
	wf->markerid = 1;
	wf->status = OPH_ODB_STATUS_RUNNING;
	wf->username = strdup("oph-test");
	wf->userrole = 31;
	wf->name = strdup("test");
	wf->author = strdup("test");
	wf->abstract = strdup("-");
	wf->sessionid = strdup(sessionid);
	wf->exec_mode = strdup("sync");
	wf->ncores = 1;
	wf->cwd = strdup("/123/");
	wf->run = 1;
	wf->parallel_mode = 0;

	if (!strcmp(operator,"oph_if"))
	{
		char condition[OPH_MAX_STRING_SIZE];
		sprintf(condition,"1");

		switch (option)
		{
			case 0:
			{
				*condition = 0;
			}
			break;

			case 2:
			{
				sprintf(condition,"0");
			}
			break;

			case 5:
			{
				sprintf(condition,"0/0");
			}
			break;

			case 6:
			{
				sprintf(condition,"1/0");
			}
			break;

			default:;
		}

		// Tasks
		wf->tasks_num = 5;
		wf->residual_tasks_num = 5;
		wf->tasks = (oph_workflow_task*)calloc(1+wf->tasks_num,sizeof(oph_workflow_task));
		wf->vars = hashtbl_create(wf->tasks_num, NULL);

		// IF
		wf->tasks[0].idjob = 2;
		wf->tasks[0].markerid = 2;
		wf->tasks[0].status = OPH_ODB_STATUS_PENDING;
		wf->tasks[0].name = strdup("IF");
		wf->tasks[0].operator = strdup("oph_if");
		wf->tasks[0].role = oph_code_role("read");
		wf->tasks[0].ncores = wf->ncores;
		wf->tasks[0].arguments_num = 1;
		wf->tasks[0].arguments_keys = (char**)calloc(wf->tasks[0].arguments_num,sizeof(char*));
			wf->tasks[0].arguments_keys[0] = strdup("condition");
		wf->tasks[0].arguments_values = (char**)calloc(wf->tasks[0].arguments_num,sizeof(char*));
			wf->tasks[0].arguments_values[0] = strdup(condition);
		wf->tasks[0].deps_num = 0;
		wf->tasks[0].deps = NULL;
		wf->tasks[0].dependents_indexes_num = 2;
		wf->tasks[0].dependents_indexes = (int*)calloc(wf->tasks[0].dependents_indexes_num,sizeof(int));
			wf->tasks[0].dependents_indexes[0] = 1;
			wf->tasks[0].dependents_indexes[1] = 2;
		wf->tasks[0].run = 1;
		wf->tasks[0].parent = -1;

		// Operator for true
		wf->tasks[1].idjob = 3;
		wf->tasks[1].markerid = 3;
		wf->tasks[1].status = OPH_ODB_STATUS_UNKNOWN;
		wf->tasks[1].name = strdup("Operator for true");
		wf->tasks[1].operator = strdup("oph_operator");
		wf->tasks[1].role = oph_code_role("read");
		wf->tasks[1].ncores = wf->ncores;
		wf->tasks[1].arguments_num = 0;
		wf->tasks[1].arguments_keys = NULL;
		wf->tasks[1].arguments_values = NULL;
		wf->tasks[1].deps = (oph_workflow_dep*)calloc(wf->tasks[1].deps_num,sizeof(oph_workflow_dep));
			wf->tasks[1].deps[0].task_name = strdup("oph_if");
			wf->tasks[1].deps[0].task_index = 0;
			wf->tasks[1].deps[0].type = strdup("embedded");
		wf->tasks[1].dependents_indexes_num = 1;
		wf->tasks[1].dependents_indexes = (int*)calloc(wf->tasks[1].dependents_indexes_num,sizeof(int));
			wf->tasks[1].dependents_indexes[0] = 4;
		wf->tasks[1].run = 1;
		wf->tasks[1].parent = -1;

		// ELSE
		wf->tasks[2].idjob = 4;
		wf->tasks[2].markerid = 4;
		wf->tasks[2].status = OPH_ODB_STATUS_UNKNOWN;
		wf->tasks[2].name = strdup("ELSE");
		wf->tasks[2].operator = strdup("oph_else");
		wf->tasks[2].role = oph_code_role("read");
		wf->tasks[2].ncores = wf->ncores;
		wf->tasks[2].arguments_num = 0;
		wf->tasks[2].arguments_keys = NULL;
		wf->tasks[2].arguments_values = NULL;
		wf->tasks[2].deps = (oph_workflow_dep*)calloc(wf->tasks[2].deps_num,sizeof(oph_workflow_dep));
			wf->tasks[2].deps[0].task_name = strdup("oph_if");
			wf->tasks[2].deps[0].task_index = 0;
			wf->tasks[2].deps[0].type = strdup("embedded");
		wf->tasks[2].dependents_indexes_num = 1;
		wf->tasks[2].dependents_indexes = (int*)calloc(wf->tasks[2].dependents_indexes_num,sizeof(int));
			wf->tasks[2].dependents_indexes[0] = 3;
		wf->tasks[2].run = 1;
		wf->tasks[2].parent = 0;

		// Operator for false
		wf->tasks[3].idjob = 5;
		wf->tasks[3].markerid = 5;
		wf->tasks[3].status = OPH_ODB_STATUS_UNKNOWN;
		wf->tasks[3].name = strdup("Operator for false");
		wf->tasks[3].operator = strdup("oph_operator");
		wf->tasks[3].role = oph_code_role("read");
		wf->tasks[3].ncores = wf->ncores;
		wf->tasks[3].arguments_num = 0;
		wf->tasks[3].arguments_keys = NULL;
		wf->tasks[3].arguments_values = NULL;
		wf->tasks[3].deps = (oph_workflow_dep*)calloc(wf->tasks[3].deps_num,sizeof(oph_workflow_dep));
			wf->tasks[3].deps[0].task_name = strdup("oph_else");
			wf->tasks[3].deps[0].task_index = 2;
			wf->tasks[3].deps[0].type = strdup("embedded");
		wf->tasks[3].dependents_indexes_num = 1;
		wf->tasks[3].dependents_indexes = (int*)calloc(wf->tasks[3].dependents_indexes_num,sizeof(int));
			wf->tasks[3].dependents_indexes[0] = 4;
		wf->tasks[3].run = 1;
		wf->tasks[3].parent = -1;

		// ENDIF
		wf->tasks[4].idjob = 6;
		wf->tasks[4].markerid = 6;
		wf->tasks[4].status = OPH_ODB_STATUS_UNKNOWN;
		wf->tasks[4].name = strdup("ENDIF");
		wf->tasks[4].operator = strdup("oph_endif");
		wf->tasks[4].role = oph_code_role("read");
		wf->tasks[4].ncores = wf->ncores;
		wf->tasks[4].arguments_num = 0;
		wf->tasks[4].arguments_keys = NULL;
		wf->tasks[4].arguments_values = NULL;
		wf->tasks[4].deps_num = 2;
		wf->tasks[4].deps = (oph_workflow_dep*)calloc(wf->tasks[4].deps_num,sizeof(oph_workflow_dep));
			wf->tasks[4].deps[0].task_name = strdup("oph_operator");
			wf->tasks[4].deps[0].task_index = 1;
			wf->tasks[4].deps[0].type = strdup("embedded");
			wf->tasks[4].deps[1].task_name = strdup("oph_operator");
			wf->tasks[4].deps[1].task_index = 3;
			wf->tasks[4].deps[1].type = strdup("embedded");
		wf->tasks[4].dependents_indexes_num = 0;
		wf->tasks[4].dependents_indexes = NULL;
		wf->tasks[4].run = 1;
		wf->tasks[4].parent = 0;

		char error_message[OPH_MAX_STRING_SIZE];
		int exit_output;
		*error_message = 0;

		switch (option)
		{
			case 3:
			{
				wf->tasks[0].is_skipped = 1; // in case of oph_elseif
			}
			break;

			case 4:
			{
				free(wf->tasks[0].arguments_keys[0]);
				free(wf->tasks[0].arguments_keys);
				free(wf->tasks[0].arguments_values[0]);
				free(wf->tasks[0].arguments_values);
				wf->tasks[0].arguments_num = 0;
				wf->tasks[0].arguments_keys = NULL;
				wf->tasks[0].arguments_values = NULL;
			}
			break;

			case 7:
			{
				oph_workflow_var var;
				var.caller = -1;
				var.ivalue = 1;
				snprintf(var.svalue,OPH_WORKFLOW_MAX_STRING,"234-234");
				if (hashtbl_insert_with_size(wf->vars, "condition", (void *)&var, sizeof(oph_workflow_var))) return 1;
				free(wf->tasks[0].arguments_values[0]);
				wf->tasks[0].arguments_values[0] = strdup("@condition");
			}
			break;

			case 8:
			{
				free(wf->tasks[0].arguments_values[0]);
				wf->tasks[0].arguments_values[0] = strdup("@condition");
			}
			break;

			default:;
		}

		int res = oph_if_impl(wf, 0, error_message, &exit_output);

		switch (option)
		{
			case 5:
				if ((res != OPH_SERVER_ERROR) || strcasecmp(error_message,"Wrong condition '0/0'!"))
				{
					pmesg(LOG_ERROR, __FILE__,__LINE__, "Error message: %s\n",error_message);
					return 1;
				}
			break;
			case 6:
				if ((res != OPH_SERVER_ERROR) || strcasecmp(error_message,"Wrong condition '1/0'!"))
				{
					pmesg(LOG_ERROR, __FILE__,__LINE__, "Error message: %s\n",error_message);
					return 1;
				}
			break;
			case 8:
				if ((res != OPH_SERVER_ERROR) || strcasecmp(error_message,"Bad variable '@condition' in task 'IF'"))
				{
					pmesg(LOG_ERROR, __FILE__,__LINE__, "Error message: %s\n",error_message);
					return 1;
				}
			break;

			default:
				if (res || strlen(error_message))
				{
					pmesg(LOG_ERROR, __FILE__,__LINE__, "Return code: %d\nError message: %s\n",res,error_message);
					return 1;
				}
		}

		switch (option)
		{
			case 0:
			case 1:
			{
				if (wf->tasks[0].is_skipped || wf->tasks[1].is_skipped || !wf->tasks[2].is_skipped || wf->tasks[3].is_skipped || wf->tasks[4].is_skipped)
				{
					pmesg(LOG_ERROR, __FILE__,__LINE__, "Skipping flags are wrong\n");
					return 1;
				}
			}
			break;

			case 2:
			case 7:
			{
				if (wf->tasks[1].status != OPH_ODB_STATUS_SKIPPED)
				{
					pmesg(LOG_ERROR, __FILE__,__LINE__, "Status flags are wrong\n");
					return 1;
				}
				if (wf->tasks[0].is_skipped || wf->tasks[1].is_skipped || wf->tasks[2].is_skipped || wf->tasks[3].is_skipped || wf->tasks[4].is_skipped)
				{
					pmesg(LOG_ERROR, __FILE__,__LINE__, "Skipping flags are wrong\n");
					return 1;
				}
				if ((wf->tasks[4].deps[0].task_index != 0) || (wf->tasks[4].deps[1].task_index != 3))
				{
					pmesg(LOG_ERROR, __FILE__,__LINE__, "Dependence data are wrong\n");
					return 1;
				}
			}
			break;

			case 3:
			{
				if (wf->tasks[1].status != OPH_ODB_STATUS_SKIPPED)
				{
					pmesg(LOG_ERROR, __FILE__,__LINE__, "Status flags are wrong\n");
					return 1;
				}
				if (!wf->tasks[0].is_skipped || wf->tasks[1].is_skipped || !wf->tasks[2].is_skipped || wf->tasks[3].is_skipped || wf->tasks[4].is_skipped)
				{
					pmesg(LOG_ERROR, __FILE__,__LINE__, "Skipping flags are wrong\n");
					return 1;
				}
				if ((wf->tasks[4].deps[0].task_index != 0) || (wf->tasks[4].deps[1].task_index != 3))
				{
					pmesg(LOG_ERROR, __FILE__,__LINE__, "Dependence data are wrong\n");
					return 1;
				}
			}
			break;

			case 4:
			{
				if (wf->tasks[0].is_skipped || wf->tasks[1].is_skipped || !wf->tasks[2].is_skipped || wf->tasks[3].is_skipped || wf->tasks[4].is_skipped)
				{
					pmesg(LOG_ERROR, __FILE__,__LINE__, "Skipping flags are wrong\n");
					return 1;
				}
			}
			break;

			default:;
		}
	}
	else if (!strcmp(operator,"oph_else"))
	{
		// Tasks
		wf->tasks_num = 5;
		wf->residual_tasks_num = 3;
		wf->tasks = (oph_workflow_task*)calloc(1+wf->tasks_num,sizeof(oph_workflow_task));
		wf->vars = hashtbl_create(wf->tasks_num, NULL);

		// IF
		wf->tasks[0].idjob = 2;
		wf->tasks[0].markerid = 2;
		wf->tasks[0].status = OPH_ODB_STATUS_COMPLETED;
		wf->tasks[0].name = strdup("IF");
		wf->tasks[0].operator = strdup("oph_if");
		wf->tasks[0].role = oph_code_role("read");
		wf->tasks[0].ncores = wf->ncores;
		wf->tasks[0].arguments_num = 1;
		wf->tasks[0].arguments_keys = (char**)calloc(wf->tasks[0].arguments_num,sizeof(char*));
			wf->tasks[0].arguments_keys[0] = strdup("condition");
		wf->tasks[0].arguments_values = (char**)calloc(wf->tasks[0].arguments_num,sizeof(char*));
			wf->tasks[0].arguments_values[0] = strdup("0");
		wf->tasks[0].deps_num = 0;
		wf->tasks[0].deps = NULL;
		wf->tasks[0].dependents_indexes_num = 2;
		wf->tasks[0].dependents_indexes = (int*)calloc(wf->tasks[0].dependents_indexes_num,sizeof(int));
			wf->tasks[0].dependents_indexes[0] = 4;
			wf->tasks[0].dependents_indexes[1] = 2;
		wf->tasks[0].run = 1;
		wf->tasks[0].parent = -1;

		// Operator for true
		wf->tasks[1].idjob = 3;
		wf->tasks[1].markerid = 3;
		wf->tasks[1].status = OPH_ODB_STATUS_SKIPPED;
		wf->tasks[1].name = strdup("Operator for true");
		wf->tasks[1].operator = strdup("oph_operator");
		wf->tasks[1].role = oph_code_role("read");
		wf->tasks[1].ncores = wf->ncores;
		wf->tasks[1].arguments_num = 0;
		wf->tasks[1].arguments_keys = NULL;
		wf->tasks[1].arguments_values = NULL;
		wf->tasks[1].deps = (oph_workflow_dep*)calloc(wf->tasks[1].deps_num,sizeof(oph_workflow_dep));
			wf->tasks[1].deps[0].task_name = strdup("oph_if");
			wf->tasks[1].deps[0].task_index = 0;
			wf->tasks[1].deps[0].type = strdup("embedded");
		wf->tasks[1].dependents_indexes_num = 1;
		wf->tasks[1].dependents_indexes = (int*)calloc(wf->tasks[1].dependents_indexes_num,sizeof(int));
			wf->tasks[1].dependents_indexes[0] = 4;
		wf->tasks[1].run = 1;
		wf->tasks[1].parent = -1;

		// ELSE
		wf->tasks[2].idjob = 4;
		wf->tasks[2].markerid = 4;
		wf->tasks[2].status = OPH_ODB_STATUS_PENDING;
		wf->tasks[2].name = strdup("ELSE");
		wf->tasks[2].operator = strdup("oph_else");
		wf->tasks[2].role = oph_code_role("read");
		wf->tasks[2].ncores = wf->ncores;
		wf->tasks[2].arguments_num = 0;
		wf->tasks[2].arguments_keys = NULL;
		wf->tasks[2].arguments_values = NULL;
		wf->tasks[2].deps = (oph_workflow_dep*)calloc(wf->tasks[2].deps_num,sizeof(oph_workflow_dep));
			wf->tasks[2].deps[0].task_name = strdup("oph_if");
			wf->tasks[2].deps[0].task_index = 0;
			wf->tasks[2].deps[0].type = strdup("embedded");
		wf->tasks[2].dependents_indexes_num = 1;
		wf->tasks[2].dependents_indexes = (int*)calloc(wf->tasks[2].dependents_indexes_num,sizeof(int));
			wf->tasks[2].dependents_indexes[0] = 3;
		wf->tasks[2].run = 1;
		wf->tasks[2].parent = 0;

		// Operator for false
		wf->tasks[3].idjob = 5;
		wf->tasks[3].markerid = 5;
		wf->tasks[3].status = OPH_ODB_STATUS_UNKNOWN;
		wf->tasks[3].name = strdup("Operator for false");
		wf->tasks[3].operator = strdup("oph_operator");
		wf->tasks[3].role = oph_code_role("read");
		wf->tasks[3].ncores = wf->ncores;
		wf->tasks[3].arguments_num = 0;
		wf->tasks[3].arguments_keys = NULL;
		wf->tasks[3].arguments_values = NULL;
		wf->tasks[3].deps = (oph_workflow_dep*)calloc(wf->tasks[3].deps_num,sizeof(oph_workflow_dep));
			wf->tasks[3].deps[0].task_name = strdup("oph_else");
			wf->tasks[3].deps[0].task_index = 2;
			wf->tasks[3].deps[0].type = strdup("embedded");
		wf->tasks[3].dependents_indexes_num = 1;
		wf->tasks[3].dependents_indexes = (int*)calloc(wf->tasks[3].dependents_indexes_num,sizeof(int));
			wf->tasks[3].dependents_indexes[0] = 4;
		wf->tasks[3].run = 1;
		wf->tasks[3].parent = -1;

		// ENDIF
		wf->tasks[4].idjob = 6;
		wf->tasks[4].markerid = 6;
		wf->tasks[4].status = OPH_ODB_STATUS_UNKNOWN;
		wf->tasks[4].name = strdup("ENDIF");
		wf->tasks[4].operator = strdup("oph_endif");
		wf->tasks[4].role = oph_code_role("read");
		wf->tasks[4].ncores = wf->ncores;
		wf->tasks[4].arguments_num = 0;
		wf->tasks[4].arguments_keys = NULL;
		wf->tasks[4].arguments_values = NULL;
		wf->tasks[4].deps_num = 2;
		wf->tasks[4].deps = (oph_workflow_dep*)calloc(wf->tasks[4].deps_num,sizeof(oph_workflow_dep));
			wf->tasks[4].deps[0].task_name = strdup("oph_operator");
			wf->tasks[4].deps[0].task_index = 0;
			wf->tasks[4].deps[0].type = strdup("embedded");
			wf->tasks[4].deps[1].task_name = strdup("oph_operator");
			wf->tasks[4].deps[1].task_index = 3;
			wf->tasks[4].deps[1].type = strdup("embedded");
		wf->tasks[4].dependents_indexes_num = 0;
		wf->tasks[4].dependents_indexes = NULL;
		wf->tasks[4].run = 1;
		wf->tasks[4].parent = 0;

		char error_message[OPH_MAX_STRING_SIZE];
		int exit_output;
		*error_message = 0;

		switch (option)
		{
			case 1:
			{
				wf->tasks[0].dependents_indexes[0] = 1;
				wf->tasks[0].dependents_indexes[1] = 4;
				wf->tasks[1].status = OPH_ODB_STATUS_PENDING;
				wf->tasks[2].is_skipped = 1;
				wf->tasks[4].deps[0].task_index = 1;
				wf->tasks[4].deps[1].task_index = 0;
			}
			break;

			default:;
		}

		int res = oph_else_impl(wf, 2, error_message, &exit_output);

		switch (option)
		{
			default:
				if (res || strlen(error_message))
				{
					pmesg(LOG_ERROR, __FILE__,__LINE__, "Return code: %d\nError message: %s\n",res,error_message);
					return 1;
				}
		}

		switch (option)
		{
			case 0:
			{
				if (wf->tasks[1].status != OPH_ODB_STATUS_SKIPPED)
				{
					pmesg(LOG_ERROR, __FILE__,__LINE__, "Status flags are wrong\n");
					return 1;
				}
				if (wf->tasks[0].is_skipped || wf->tasks[1].is_skipped || wf->tasks[2].is_skipped || wf->tasks[3].is_skipped || wf->tasks[4].is_skipped)
				{
					pmesg(LOG_ERROR, __FILE__,__LINE__, "Skipping flags are wrong\n");
					return 1;
				}
				if ((wf->tasks[4].deps[0].task_index != 0) || (wf->tasks[4].deps[1].task_index != 3))
				{
					pmesg(LOG_ERROR, __FILE__,__LINE__, "Dependence data are wrong\n");
					return 1;
				}
			}
			break;

			case 1:
			{
				if ((wf->tasks[1].status == OPH_ODB_STATUS_SKIPPED) || (wf->tasks[3].status != OPH_ODB_STATUS_SKIPPED))
				{
					pmesg(LOG_ERROR, __FILE__,__LINE__, "Status flags are wrong\n");
					return 1;
				}
				if (wf->tasks[0].is_skipped || wf->tasks[1].is_skipped || !wf->tasks[2].is_skipped || wf->tasks[3].is_skipped || wf->tasks[4].is_skipped)
				{
					pmesg(LOG_ERROR, __FILE__,__LINE__, "Skipping flags are wrong\n");
					return 1;
				}
				if ((wf->tasks[4].deps[0].task_index != 1) || (wf->tasks[4].deps[1].task_index != 0))
				{
					pmesg(LOG_ERROR, __FILE__,__LINE__, "Dependence data are wrong\n");
					return 1;
				}
			}
			break;

			default:;
		}
	}
	else if (!strcmp(operator,"oph_for"))
	{
		// Tasks
		wf->tasks_num = 3;
		wf->residual_tasks_num = 3;
		wf->tasks = (oph_workflow_task*)calloc(1+wf->tasks_num,sizeof(oph_workflow_task));
		wf->vars = hashtbl_create(wf->tasks_num, NULL);

		// FOR
		wf->tasks[0].idjob = 2;
		wf->tasks[0].markerid = 2;
		wf->tasks[0].status = OPH_ODB_STATUS_PENDING;
		wf->tasks[0].name = strdup("FOR");
		wf->tasks[0].operator = strdup("oph_for");
		wf->tasks[0].role = oph_code_role("read");
		wf->tasks[0].ncores = wf->ncores;
		wf->tasks[0].arguments_num = 4;
		wf->tasks[0].arguments_keys = (char**)calloc(wf->tasks[0].arguments_num,sizeof(char*));
			wf->tasks[0].arguments_keys[0] = strdup("name");
			wf->tasks[0].arguments_keys[1] = strdup("values");
			wf->tasks[0].arguments_keys[2] = strdup("counter");
			wf->tasks[0].arguments_keys[3] = strdup("parallel");
		wf->tasks[0].arguments_values = (char**)calloc(wf->tasks[0].arguments_num,sizeof(char*));
			wf->tasks[0].arguments_values[0] = strdup("index");
			wf->tasks[0].arguments_values[1] = strdup("first|second|third");
			wf->tasks[0].arguments_values[2] = strdup("1:3");
			wf->tasks[0].arguments_values[3] = strdup("no");
		wf->tasks[0].deps_num = 0;
		wf->tasks[0].deps = NULL;
		wf->tasks[0].dependents_indexes_num = 1;
		wf->tasks[0].dependents_indexes = (int*)calloc(wf->tasks[0].dependents_indexes_num,sizeof(int));
			wf->tasks[0].dependents_indexes[0] = 1;
		wf->tasks[0].run = 1;
		wf->tasks[0].parent = -1;

		// Operator
		wf->tasks[1].idjob = 3;
		wf->tasks[1].markerid = 3;
		wf->tasks[1].status = OPH_ODB_STATUS_UNKNOWN;
		wf->tasks[1].name = strdup("Operator");
		wf->tasks[1].operator = strdup("oph_operator");
		wf->tasks[1].role = oph_code_role("read");
		wf->tasks[1].ncores = wf->ncores;
		wf->tasks[1].arguments_num = 0;
		wf->tasks[1].arguments_keys = NULL;
		wf->tasks[1].arguments_values = NULL;
		wf->tasks[1].deps = (oph_workflow_dep*)calloc(wf->tasks[1].deps_num,sizeof(oph_workflow_dep));
			wf->tasks[1].deps[0].task_name = strdup("oph_for");
			wf->tasks[1].deps[0].task_index = 0;
			wf->tasks[1].deps[0].type = strdup("embedded");
		wf->tasks[1].dependents_indexes_num = 1;
		wf->tasks[1].dependents_indexes = (int*)calloc(wf->tasks[1].dependents_indexes_num,sizeof(int));
			wf->tasks[1].dependents_indexes[0] = 2;
		wf->tasks[1].run = 1;
		wf->tasks[1].parent = -1;

		// ENDFOR
		wf->tasks[2].idjob = 4;
		wf->tasks[2].markerid = 4;
		wf->tasks[2].status = OPH_ODB_STATUS_UNKNOWN;
		wf->tasks[2].name = strdup("ENDFOR");
		wf->tasks[2].operator = strdup("oph_endfor");
		wf->tasks[2].role = oph_code_role("read");
		wf->tasks[2].ncores = wf->ncores;
		wf->tasks[2].arguments_num = 0;
		wf->tasks[2].arguments_keys = NULL;
		wf->tasks[2].arguments_values = NULL;
		wf->tasks[2].deps = (oph_workflow_dep*)calloc(wf->tasks[2].deps_num,sizeof(oph_workflow_dep));
			wf->tasks[2].deps[0].task_name = strdup("oph_operator");
			wf->tasks[2].deps[0].task_index = 1;
			wf->tasks[2].deps[0].type = strdup("embedded");
		wf->tasks[2].dependents_indexes_num = 0;
		wf->tasks[2].dependents_indexes = NULL;
		wf->tasks[2].run = 1;
		wf->tasks[2].parent = 0;

		char error_message[OPH_MAX_STRING_SIZE];
		*error_message = 0;

		switch (option)
		{
			case 1:
			{
				oph_workflow_var var;
				var.caller = -1;
				var.ivalue = 1;
				snprintf(var.svalue,OPH_WORKFLOW_MAX_STRING,"first|second|third");
				if (hashtbl_insert_with_size(wf->vars, "values", (void *)&var, sizeof(oph_workflow_var))) return 1;
				free(wf->tasks[0].arguments_values[1]);
				wf->tasks[0].arguments_values[1] = strdup("@values");
			}
			break;

			case 2:
			{
				free(wf->tasks[0].arguments_keys[0]);
				wf->tasks[0].arguments_keys[0] = strdup("no-name");
			}
			break;

			case 3:
			{
				free(wf->tasks[0].arguments_keys[1]);
				wf->tasks[0].arguments_keys[1] = strdup("no-values");
			}
			break;

			case 4:
			{
				free(wf->tasks[0].arguments_keys[2]);
				wf->tasks[0].arguments_keys[2] = strdup("no-counter");
			}
			break;

			case 5:
			{
				free(wf->tasks[0].arguments_keys[3]);
				wf->tasks[0].arguments_keys[3] = strdup("no-parallel");
			}
			break;

			case 6:
			{
				free(wf->tasks[0].arguments_keys[1]);
				wf->tasks[0].arguments_keys[1] = strdup("no-values");
				free(wf->tasks[0].arguments_keys[2]);
				wf->tasks[0].arguments_keys[2] = strdup("no-counter");
			}
			break;

			case 7:
			{
				free(wf->tasks[0].arguments_values[3]);
				wf->tasks[0].arguments_values[3] = strdup("yes");
			}
			break;

			case 8:
			{
				free(wf->tasks[0].arguments_values[0]);
				wf->tasks[0].arguments_values[0] = strdup("1ndex");
			}
			break;

			default:;
		}

		int res = oph_for_impl(wf, 0, error_message, 1);

		switch (option)
		{
			case 2:
				if ((res != OPH_SERVER_ERROR) || strcasecmp(error_message,"Bad argument 'name'."))
				{
					pmesg(LOG_ERROR, __FILE__,__LINE__, "Error message: %s\n",error_message);
					return 1;
				}
			break;

			case 7:
				if (res || strlen(error_message))
				{
					pmesg(LOG_ERROR, __FILE__,__LINE__, "Return code: %d\nError message: %s\n",res,error_message);
					return 1;
				}
				if (wf->stack)
				{
					pmesg(LOG_ERROR, __FILE__,__LINE__, "Non-empty stack\n");
					return 1;
				}
			break;

			case 8:
				if (res || !strlen(error_message))
				{
					pmesg(LOG_ERROR, __FILE__,__LINE__, "Return code: %d\nEmpty error message\n",res);
					return 1;
				}
				if (strcasecmp(error_message,"Change variable name '1ndex'."))
				{
					pmesg(LOG_ERROR, __FILE__,__LINE__, "Wrong error message: %s\n",error_message);
					return 1;
				}
				if (!wf->stack)
				{
					pmesg(LOG_ERROR, __FILE__,__LINE__, "Empty stack\n");
					return 1;
				}
				if (wf->stack->caller)
				{
					pmesg(LOG_ERROR, __FILE__,__LINE__, "Flag 'caller' is wrong\n");
					return 1;
				}
				if (wf->stack->index)
				{
					pmesg(LOG_ERROR, __FILE__,__LINE__, "Index is wrong\n");
					return 1;
				}
				if (!wf->stack->name)
				{
					pmesg(LOG_ERROR, __FILE__,__LINE__, "Parameters are not correctly pushed into the stack\n");
					return 1;
				}
			break;

			default:
				if (res || strlen(error_message))
				{
					pmesg(LOG_ERROR, __FILE__,__LINE__, "Return code: %d\nError message: %s\n",res,error_message);
					return 1;
				}
				if (!wf->stack)
				{
					pmesg(LOG_ERROR, __FILE__,__LINE__, "Empty stack\n");
					return 1;
				}
				if (wf->stack->caller)
				{
					pmesg(LOG_ERROR, __FILE__,__LINE__, "Flag 'caller' is wrong\n");
					return 1;
				}
				if (wf->stack->index)
				{
					pmesg(LOG_ERROR, __FILE__,__LINE__, "Index is wrong\n");
					return 1;
				}
				if (!wf->stack->name)
				{
					pmesg(LOG_ERROR, __FILE__,__LINE__, "Parameters are not correctly pushed into the stack\n");
					return 1;
				}
		}

		switch (option)
		{
			case 0:
			case 1:
			case 4:
			case 5:
			{
				if (!wf->stack->svalues || (wf->stack->values_num != 3))
				{
					pmesg(LOG_ERROR, __FILE__,__LINE__, "Parameters are not correctly pushed into the stack\n");
					return 1;
				}
				if (strcasecmp(wf->stack->svalues[0],"first") || strcasecmp(wf->stack->svalues[1],"second") || strcasecmp(wf->stack->svalues[2],"third"))
				{
					pmesg(LOG_ERROR, __FILE__,__LINE__, "Parameters are not correctly pushed into the stack\n");
					return 1;
				}
			}
			break;
		}

		switch (option)
		{
			case 0:
			case 1:
			case 3:
			case 5:
			{
				if (!wf->stack->ivalues || (wf->stack->values_num != 3))
				{
					pmesg(LOG_ERROR, __FILE__,__LINE__, "Parameters are not correctly pushed into the stack\n");
					return 1;
				}
				if ((wf->stack->ivalues[0] != 1) || (wf->stack->ivalues[1] != 2) || (wf->stack->ivalues[2] != 3))
				{
					pmesg(LOG_ERROR, __FILE__,__LINE__, "Parameters are not correctly pushed into the stack\n");
					return 1;
				}
			}
			break;
		}

		switch (option)
		{
			case 6:
			{
				if ((wf->stack->values_num != 1) || wf->stack->ivalues || wf->stack->svalues)
				{
					pmesg(LOG_ERROR, __FILE__,__LINE__, "Parameters are not correctly pushed into the stack\n");
					return 1;
				}
			}
			break;
		}

	}
	else if (!strcmp(operator,"oph_endfor"))
	{
		// Tasks
		wf->tasks_num = 3;
		wf->residual_tasks_num = 1;
		wf->tasks = (oph_workflow_task*)calloc(1+wf->tasks_num,sizeof(oph_workflow_task));
		wf->vars = hashtbl_create(wf->tasks_num, NULL);

		// FOR
		wf->tasks[0].idjob = 2;
		wf->tasks[0].markerid = 2;
		wf->tasks[0].status = OPH_ODB_STATUS_COMPLETED;
		wf->tasks[0].name = strdup("FOR");
		wf->tasks[0].operator = strdup("oph_for");
		wf->tasks[0].role = oph_code_role("read");
		wf->tasks[0].ncores = wf->ncores;
		wf->tasks[0].arguments_num = 4;
		wf->tasks[0].arguments_keys = (char**)calloc(wf->tasks[0].arguments_num,sizeof(char*));
			wf->tasks[0].arguments_keys[0] = strdup("name");
			wf->tasks[0].arguments_keys[1] = strdup("values");
			wf->tasks[0].arguments_keys[2] = strdup("counter");
			wf->tasks[0].arguments_keys[3] = strdup("parallel");
		wf->tasks[0].arguments_values = (char**)calloc(wf->tasks[0].arguments_num,sizeof(char*));
			wf->tasks[0].arguments_values[0] = strdup("index");
			wf->tasks[0].arguments_values[1] = strdup("first|second|third");
			wf->tasks[0].arguments_values[2] = strdup("1:3");
			wf->tasks[0].arguments_values[3] = strdup("no");
		wf->tasks[0].deps_num = 0;
		wf->tasks[0].deps = NULL;
		wf->tasks[0].dependents_indexes_num = 1;
		wf->tasks[0].dependents_indexes = (int*)calloc(wf->tasks[0].dependents_indexes_num,sizeof(int));
			wf->tasks[0].dependents_indexes[0] = 1;
		wf->tasks[0].run = 1;
		wf->tasks[0].parent = -1;
		wf->tasks[0].outputs_num = 1;
		wf->tasks[0].outputs_keys = (char**)calloc(wf->tasks[0].outputs_num,sizeof(char*));
			wf->tasks[0].outputs_keys[0] = strdup("output_key");
		wf->tasks[0].outputs_values = (char**)calloc(wf->tasks[0].outputs_num,sizeof(char*));
			wf->tasks[0].outputs_values[0] = strdup("output_value");

		// Operator
		wf->tasks[1].idjob = 3;
		wf->tasks[1].markerid = 3;
		wf->tasks[1].status = OPH_ODB_STATUS_COMPLETED;
		wf->tasks[1].name = strdup("Operator");
		wf->tasks[1].operator = strdup("oph_operator");
		wf->tasks[1].role = oph_code_role("read");
		wf->tasks[1].ncores = wf->ncores;
		wf->tasks[1].arguments_num = 0;
		wf->tasks[1].arguments_keys = NULL;
		wf->tasks[1].arguments_values = NULL;
		wf->tasks[1].deps = (oph_workflow_dep*)calloc(wf->tasks[1].deps_num,sizeof(oph_workflow_dep));
			wf->tasks[1].deps[0].task_name = strdup("oph_for");
			wf->tasks[1].deps[0].task_index = 0;
			wf->tasks[1].deps[0].type = strdup("embedded");
		wf->tasks[1].dependents_indexes_num = 1;
		wf->tasks[1].dependents_indexes = (int*)calloc(wf->tasks[1].dependents_indexes_num,sizeof(int));
			wf->tasks[1].dependents_indexes[0] = 2;
		wf->tasks[1].run = 1;
		wf->tasks[1].parent = -1;

		// ENDFOR
		wf->tasks[2].idjob = 4;
		wf->tasks[2].markerid = 4;
		wf->tasks[2].status = OPH_ODB_STATUS_PENDING;
		wf->tasks[2].name = strdup("ENDFOR");
		wf->tasks[2].operator = strdup("oph_endfor");
		wf->tasks[2].role = oph_code_role("read");
		wf->tasks[2].ncores = wf->ncores;
		wf->tasks[2].arguments_num = 0;
		wf->tasks[2].arguments_keys = NULL;
		wf->tasks[2].arguments_values = NULL;
		wf->tasks[2].deps = (oph_workflow_dep*)calloc(wf->tasks[2].deps_num,sizeof(oph_workflow_dep));
			wf->tasks[2].deps[0].task_name = strdup("oph_operator");
			wf->tasks[2].deps[0].task_index = 1;
			wf->tasks[2].deps[0].type = strdup("embedded");
		wf->tasks[2].dependents_indexes_num = 0;
		wf->tasks[2].dependents_indexes = NULL;
		wf->tasks[2].run = 1;
		wf->tasks[2].parent = 0;

		char error_message[OPH_MAX_STRING_SIZE];
		*error_message = 0;

		int task_id = 2;
		int odb_jobid = wf->tasks[2].idjob;

		oph_trash* trash;
		if (oph_trash_create(&trash)) return 1;

		switch (option)
		{
			case 3:
			{

			}
			break;

			case 4:
			{
				int svalues_num = 3;
				char** svalues = (char**)calloc(svalues_num,sizeof(char*));
				svalues[0] = strdup("first");
				svalues[1] = strdup("second");
				svalues[2] = strdup("third");
				int *ivalues = (int*)calloc(svalues_num,sizeof(int));
				ivalues[0] = 1;
				ivalues[1] = 2;
				ivalues[2] = 3;
				if (oph_workflow_push(wf,0,wf->tasks[0].arguments_values[0],svalues,ivalues,svalues_num)) return 1;
			}
			break;

			case 5:
			{
				int svalues_num = 3;
				char** svalues = (char**)calloc(svalues_num,sizeof(char*));
				svalues[0] = strdup("first");
				svalues[1] = strdup("second");
				svalues[2] = strdup("third");
				int *ivalues = (int*)calloc(svalues_num,sizeof(int));
				ivalues[0] = 1;
				ivalues[1] = 2;
				ivalues[2] = 3;
				if (oph_workflow_push(wf,0,wf->tasks[0].arguments_values[0],svalues,ivalues,svalues_num) || !wf->stack) return 1;

				wf->stack->index = 2;

				oph_workflow_var var;
				var.caller = 0;
				var.ivalue = ivalues[wf->stack->index];
				snprintf(var.svalue,OPH_WORKFLOW_MAX_STRING,svalues[wf->stack->index]);
				if (hashtbl_insert_with_size(wf->vars, wf->tasks[0].arguments_values[0], (void *)&var, sizeof(oph_workflow_var))) return 1;
			}
			break;

			default:
			{
				int svalues_num = 3;
				char** svalues = (char**)calloc(svalues_num,sizeof(char*));
				svalues[0] = strdup("first");
				svalues[1] = strdup("second");
				svalues[2] = strdup("third");
				int *ivalues = (int*)calloc(svalues_num,sizeof(int));
				ivalues[0] = 1;
				ivalues[1] = 2;
				ivalues[2] = 3;
				if (oph_workflow_push(wf,0,wf->tasks[0].arguments_values[0],svalues,ivalues,svalues_num) || !wf->stack) return 1;

				oph_workflow_var var;
				var.caller = 0;
				var.ivalue = ivalues[wf->stack->index];
				snprintf(var.svalue,OPH_WORKFLOW_MAX_STRING,svalues[wf->stack->index]);
				if (hashtbl_insert_with_size(wf->vars, wf->tasks[0].arguments_values[0], (void *)&var, sizeof(oph_workflow_var))) return 1;
			}
		}

		switch (option)
		{
			case 1:
			{
				free(wf->tasks[0].arguments_keys[1]);
				wf->tasks[0].arguments_keys[1] = strdup("no-values");
			}
			break;

			case 2:
			{
				free(wf->tasks[0].arguments_keys[2]);
				wf->tasks[0].arguments_keys[2] = strdup("no-counter");
			}
			break;

			default:;
		}

		int res = oph_endfor_impl(wf, 2, error_message, trash, &task_id, &odb_jobid);

		switch (option)
		{
			case 3:
			case 4:
			case 5:
				if (trash && trash->trash)
				{
					pmesg(LOG_ERROR, __FILE__,__LINE__, "Non empty trash\n");
					return 1;
				}
			break;

			default:
				if (!trash || !trash->trash || !trash->trash->key || !trash->trash->head || !trash->trash->head->item)
				{
					pmesg(LOG_ERROR, __FILE__,__LINE__, "Empty trash\n");
					return 1;
				}
				if (strcasecmp(trash->trash->key,wf->sessionid) || (trash->trash->head->item != 4))
				{
					pmesg(LOG_ERROR, __FILE__,__LINE__, "Untrashed marker id\n");
					return 1;
				}
		}

		oph_trash_destroy(trash);

		switch (option)
		{
			case 3:
			{
				if (res || !strlen(error_message))
				{
					pmesg(LOG_ERROR, __FILE__,__LINE__, "Return code: %d\nError message: %s\n",res,error_message);
					return 1;
				}
				if (strcasecmp(error_message,"No index found in environment of workflow 'test'."))
				{
					pmesg(LOG_ERROR, __FILE__,__LINE__, "Wrong error message: %s\n",error_message);
					return 1;
				}
			}
			break;

			case 4:
			{
				if ((res != OPH_SERVER_SYSTEM_ERROR) || !strlen(error_message))
				{
					pmesg(LOG_ERROR, __FILE__,__LINE__, "Return code: %d\nError message: %s\n",res,error_message);
					return 1;
				}
				if (strcasecmp(error_message,"Unable to remove variable 'index' from environment of workflow 'test'."))
				{
					pmesg(LOG_ERROR, __FILE__,__LINE__, "Wrong error message: %s\n",error_message);
					return 1;
				}
			}
			break;

			case 5:
			{
				if (res || strlen(error_message))
				{
					pmesg(LOG_ERROR, __FILE__,__LINE__, "Return code: %d\nError message: %s\n",res,error_message);
					return 1;
				}
			}
			break;

			default:
				if ((res != OPH_SERVER_NO_RESPONSE) || strlen(error_message))
				{
					pmesg(LOG_ERROR, __FILE__,__LINE__, "Return code: %d\nError message: %s\n",res,error_message);
					return 1;
				}
				if (!wf->stack)
				{
					pmesg(LOG_ERROR, __FILE__,__LINE__, "Empty stack\n");
					return 1;
				}
				if (wf->stack->caller)
				{
					pmesg(LOG_ERROR, __FILE__,__LINE__, "Flag 'caller' is wrong\n");
					return 1;
				}
				if (wf->stack->index != 1)
				{
					pmesg(LOG_ERROR, __FILE__,__LINE__, "Index is wrong\n");
					return 1;
				}
				if (!wf->stack->name)
				{
					pmesg(LOG_ERROR, __FILE__,__LINE__, "Parameters are not correctly pushed into the stack\n");
					return 1;
				}
				if (wf->tasks[0].outputs_num || wf->tasks[0].outputs_keys || wf->tasks[0].outputs_values)
				{
					pmesg(LOG_ERROR, __FILE__,__LINE__, "Task status not reset\n");
					return 1;
				}
		}

		switch (option)
		{
			case 0:
			case 2:
			{
				if (!wf->stack->svalues || (wf->stack->values_num != 3))
				{
					pmesg(LOG_ERROR, __FILE__,__LINE__, "Parameters are not correctly pushed into the stack\n");
					return 1;
				}
				if (strcasecmp(wf->stack->svalues[0],"first") || strcasecmp(wf->stack->svalues[1],"second") || strcasecmp(wf->stack->svalues[2],"third"))
				{
					pmesg(LOG_ERROR, __FILE__,__LINE__, "Parameters are not correctly pushed into the stack\n");
					return 1;
				}
			}
			break;
		}

		switch (option)
		{
			case 0:
			case 1:
			{
				if (!wf->stack->ivalues || (wf->stack->values_num != 3))
				{
					pmesg(LOG_ERROR, __FILE__,__LINE__, "Parameters are not correctly pushed into the stack\n");
					return 1;
				}
				if ((wf->stack->ivalues[0] != 1) || (wf->stack->ivalues[1] != 2) || (wf->stack->ivalues[2] != 3))
				{
					pmesg(LOG_ERROR, __FILE__,__LINE__, "Parameters are not correctly pushed into the stack\n");
					return 1;
				}
			}
			break;
		}
	}
	else if (!strcmp(operator,"oph_massive"))
	{
		int filter_num = 18;
		char *filter[] = {
			"[*]",
			"[run=no]",
			"[measure=measure]",
			"[container=containername]",
			"[cube_filter=2]",
			"[cube_filter=2:4]",
			"[cube_filter=2:3:10]",
			"[cube_filter=2,3,10]",
			"[metadata_key=key1|key2]",
			"[metadata_value=value1|value2]",
			"[metadata_key=key;metadata_value=value]",
			"[metadata_key=key1|key2;metadata_value=value1|value2]",
			"[level=2|3]",
			"[path=/path/to/container]",
			"[path=/path/to/container;recursive=yes]",
			"[container=containername;metadata_key=key;metadata_value=value;level=2;path=/path/to/container;recursive=yes]",
			"1|3|5",
			"[10]"
		};
		char *equery[] = {
			"SELECT DISTINCT datacube.iddatacube, datacube.idcontainer FROM datacube,container WHERE datacube.idcontainer=container.idcontainer AND (container.idfolder='1')",
			"SELECT DISTINCT datacube.iddatacube, datacube.idcontainer FROM datacube,container WHERE datacube.idcontainer=container.idcontainer AND (container.idfolder='1')",
			"SELECT DISTINCT datacube.iddatacube, datacube.idcontainer FROM datacube,container WHERE datacube.idcontainer=container.idcontainer AND datacube.measure='measure' AND (container.idfolder='1')",
			"SELECT DISTINCT datacube.iddatacube, datacube.idcontainer FROM datacube,container WHERE datacube.idcontainer=container.idcontainer AND container.containername='containername' AND (container.idfolder='1')",
			"SELECT DISTINCT datacube.iddatacube, datacube.idcontainer FROM datacube,container WHERE datacube.idcontainer=container.idcontainer AND (mysql.oph_is_in_subset(datacube.iddatacube,2,1,2)) AND (container.idfolder='1')",
			"SELECT DISTINCT datacube.iddatacube, datacube.idcontainer FROM datacube,container WHERE datacube.idcontainer=container.idcontainer AND (mysql.oph_is_in_subset(datacube.iddatacube,2,1,4)) AND (container.idfolder='1')",
			"SELECT DISTINCT datacube.iddatacube, datacube.idcontainer FROM datacube,container WHERE datacube.idcontainer=container.idcontainer AND (mysql.oph_is_in_subset(datacube.iddatacube,2,3,10)) AND (container.idfolder='1')",
			"SELECT DISTINCT datacube.iddatacube, datacube.idcontainer FROM datacube,container WHERE datacube.idcontainer=container.idcontainer AND (mysql.oph_is_in_subset(datacube.iddatacube,2,1,2) OR mysql.oph_is_in_subset(datacube.iddatacube,3,1,3) OR mysql.oph_is_in_subset(datacube.iddatacube,10,1,10)) AND (container.idfolder='1')",
			"SELECT DISTINCT datacube.iddatacube, datacube.idcontainer FROM datacube,container,metadatakey AS metadatakey0,metadatainstance AS metadatainstance0,metadatakey AS metadatakey1,metadatainstance AS metadatainstance1 WHERE datacube.idcontainer=container.idcontainer AND metadatakey0.idkey=metadatainstance0.idkey AND metadatainstance0.iddatacube=datacube.iddatacube AND metadatakey0.label='key1' AND metadatakey1.idkey=metadatainstance1.idkey AND metadatainstance1.iddatacube=datacube.iddatacube AND metadatakey1.label='key2' AND (container.idfolder='1')",
			"No query expected",
			"SELECT DISTINCT datacube.iddatacube, datacube.idcontainer FROM datacube,container,metadatakey AS metadatakey0k0,metadatainstance AS metadatainstance0k0 WHERE datacube.idcontainer=container.idcontainer AND metadatakey0k0.idkey=metadatainstance0k0.idkey AND metadatainstance0k0.iddatacube=datacube.iddatacube AND metadatakey0k0.label='key' AND CONVERT(metadatainstance0k0.value USING latin1) LIKE '%value%' AND (container.idfolder='1')",
			"SELECT DISTINCT datacube.iddatacube, datacube.idcontainer FROM datacube,container,metadatakey AS metadatakey0k0,metadatainstance AS metadatainstance0k0,metadatakey AS metadatakey0k1,metadatainstance AS metadatainstance0k1 WHERE datacube.idcontainer=container.idcontainer AND metadatakey0k0.idkey=metadatainstance0k0.idkey AND metadatainstance0k0.iddatacube=datacube.iddatacube AND metadatakey0k0.label='key1' AND CONVERT(metadatainstance0k0.value USING latin1) LIKE '%value1%' AND metadatakey0k1.idkey=metadatainstance0k1.idkey AND metadatainstance0k1.iddatacube=datacube.iddatacube AND metadatakey0k1.label='key2' AND CONVERT(metadatainstance0k1.value USING latin1) LIKE '%value2%' AND (container.idfolder='1')",
			"SELECT DISTINCT datacube.iddatacube, datacube.idcontainer FROM datacube,container WHERE datacube.idcontainer=container.idcontainer AND (datacube.level='2' OR datacube.level='3') AND (container.idfolder='1')",
			"SELECT DISTINCT datacube.iddatacube, datacube.idcontainer FROM datacube,container WHERE datacube.idcontainer=container.idcontainer AND (container.idfolder='1')",
			"SELECT DISTINCT datacube.iddatacube, datacube.idcontainer FROM datacube,container WHERE datacube.idcontainer=container.idcontainer AND (container.idfolder='1' OR container.idfolder='2')",
			"SELECT DISTINCT datacube.iddatacube, datacube.idcontainer FROM datacube,container,metadatakey AS metadatakey0k0,metadatainstance AS metadatainstance0k0 WHERE datacube.idcontainer=container.idcontainer AND (datacube.level='2') AND container.containername='containername' AND metadatakey0k0.idkey=metadatainstance0k0.idkey AND metadatainstance0k0.iddatacube=datacube.iddatacube AND metadatakey0k0.label='key' AND CONVERT(metadatainstance0k0.value USING latin1) LIKE '%value%' AND (container.idfolder='1' OR container.idfolder='2')",
			"No query expected",
			"SELECT DISTINCT datacube.iddatacube, datacube.idcontainer FROM datacube,container WHERE datacube.idcontainer=container.idcontainer AND (mysql.oph_is_in_subset(datacube.iddatacube,10,1,10)) AND (container.idfolder='1')"
		};

		// Tasks
		wf->tasks_num = 1;
		wf->residual_tasks_num = 1;
		wf->tasks = (oph_workflow_task*)calloc(1+wf->tasks_num,sizeof(oph_workflow_task));

		// MASSIVE
		wf->tasks[0].idjob = 2;
		wf->tasks[0].markerid = 2;
		wf->tasks[0].status = OPH_ODB_STATUS_PENDING;
		wf->tasks[0].name = strdup("MASSIVE");
		wf->tasks[0].operator = strdup("oph_massive");
		wf->tasks[0].role = oph_code_role("read");
		wf->tasks[0].ncores = wf->ncores;
		wf->tasks[0].arguments_num = 2;
		wf->tasks[0].arguments_keys = (char**)calloc(wf->tasks[0].arguments_num,sizeof(char*));
			wf->tasks[0].arguments_keys[0] = strdup("cube");
			wf->tasks[0].arguments_keys[1] = strdup("cwd");
		wf->tasks[0].arguments_values = (char**)calloc(wf->tasks[0].arguments_num,sizeof(char*));
			wf->tasks[0].arguments_values[0] = strdup(filter[option < filter_num ? option : 0]);
			wf->tasks[0].arguments_values[1] = strdup(wf->cwd);
		wf->tasks[0].run = 1;

		ophidiadb oDB;
		oph_odb_initialize_ophidiadb(&oDB);

		char** output_list = NULL, *query = NULL;
		int res, j, output_list_dim = 0;

		switch (option-filter_num)
		{
			case 0:
				res = oph_check_for_massive_operation('T', 0, NULL, 0, &oDB, &output_list, &output_list_dim, &query);
			break;

			case 1:
				res = oph_check_for_massive_operation('T', 0, wf, 0, NULL, &output_list, &output_list_dim, &query);
			break;

			case 2:
				res = oph_check_for_massive_operation('T', 0, wf, 0, &oDB, NULL, &output_list_dim, &query);
			break;

			case 3:
				res = oph_check_for_massive_operation('T', 0, wf, 0, &oDB, &output_list, NULL, &query);
			break;

			case 4:
				res = oph_check_for_massive_operation('T', 0, wf, 2, &oDB, &output_list, &output_list_dim, &query);
			break;

			case 5:
				res = oph_check_for_massive_operation('T', 0, wf, -1, &oDB, &output_list, &output_list_dim, &query);
			break;

			case 6:
				wf->tasks[0].light_tasks_num = 1;
				res = oph_check_for_massive_operation('T', 0, wf, 0, &oDB, &output_list, &output_list_dim, &query);
			break;

			case 7:
				free(wf->tasks[0].arguments_values[0]);
				wf->tasks[0].arguments_values[0] = strdup("[filter=@badvariable]");
				res = oph_check_for_massive_operation('T', 0, wf, 0, &oDB, &output_list, &output_list_dim, &query);
			break;

			case 8:
				free(wf->tasks[0].arguments_keys[0]);
				wf->tasks[0].arguments_keys[0] = strdup("cube2");
				res = oph_check_for_massive_operation('T', 0, wf, 0, &oDB, &output_list, &output_list_dim, &query);
			break;

			default:
				res = oph_check_for_massive_operation('T', 0, wf, 0, &oDB, &output_list, &output_list_dim, &query);
		}

		switch (option)
		{
			case 1:
				if (res != OPH_SERVER_NO_RESPONSE)
				{
					pmesg(LOG_ERROR, __FILE__,__LINE__, "Return code: %d\n",res);
					return 1;
				}
				if (!query)
				{
					pmesg(LOG_ERROR, __FILE__,__LINE__, "Expected return query\n");
					return 1;
				}
				if (strcasecmp(query, equery[option < filter_num ? option : 0]))
				{
					pmesg(LOG_ERROR, __FILE__,__LINE__, "Wrong return query: %s\n",query);
					return 1;
				}
			break;

			case 9:
				if (res != OPH_SERVER_SYSTEM_ERROR)
				{
					pmesg(LOG_ERROR, __FILE__,__LINE__, "Return code: %d\n",res);
					return 1;
				}
				if (query)
				{
					pmesg(LOG_ERROR, __FILE__,__LINE__, "No query expected\n");
					return 1;
				}
			break;

			case 16:
				if (res)
				{
					pmesg(LOG_ERROR, __FILE__,__LINE__, "Return code: %d\n",res);
					return 1;
				}
				if (query)
				{
					pmesg(LOG_ERROR, __FILE__,__LINE__, "No query expected\n");
					return 1;
				}
			break;

			case 18:
			case 19:
			case 20:
			case 21:
				if (res != OPH_SERVER_NULL_POINTER)
				{
					pmesg(LOG_ERROR, __FILE__,__LINE__, "Return code: %d\n",res);
					return 1;
				}
				if (query)
				{
					pmesg(LOG_ERROR, __FILE__,__LINE__, "No query expected\n");
					return 1;
				}
			break;

			case 22:
			case 23:
			case 25:
				if (res != OPH_SERVER_SYSTEM_ERROR)
				{
					pmesg(LOG_ERROR, __FILE__,__LINE__, "Return code: %d\n",res);
					return 1;
				}
				if (query)
				{
					pmesg(LOG_ERROR, __FILE__,__LINE__, "No query expected\n");
					return 1;
				}
			break;

			case 24:
				if (res != OPH_SERVER_NO_RESPONSE)
				{
					pmesg(LOG_ERROR, __FILE__,__LINE__, "Return code: %d\n",res);
					return 1;
				}
				if (query)
				{
					pmesg(LOG_ERROR, __FILE__,__LINE__, "No query expected\n");
					return 1;
				}
			break;

			case 26:
				if (res)
				{
					pmesg(LOG_ERROR, __FILE__,__LINE__, "Return code: %d\n",res);
					return 1;
				}
				if (query)
				{
					pmesg(LOG_ERROR, __FILE__,__LINE__, "Expected return query\n");
					return 1;
				}
			break;

			default:
				if (res)
				{
					pmesg(LOG_ERROR, __FILE__,__LINE__, "Return code: %d\n",res);
					return 1;
				}
				if (!query)
				{
					pmesg(LOG_ERROR, __FILE__,__LINE__, "Expected return query\n");
					return 1;
				}
				if (strcasecmp(query, equery[option < filter_num ? option : 0]))
				{
					pmesg(LOG_ERROR, __FILE__,__LINE__, "Wrong return query: %s\n",query);
					return 1;
				}
		}

		char object_name[OPH_MAX_STRING_SIZE];
		if (option < filter_num) switch (option)
		{
			case 1:
				if (output_list_dim != 3)
				{
					pmesg(LOG_ERROR, __FILE__,__LINE__, "Bad number of objects returned from the function: %d\n",output_list_dim);
					return 1;
				}
				if (!output_list)
				{
					pmesg(LOG_ERROR, __FILE__,__LINE__, "Bad object list returned from the function\n");
					return 1;
				}
				for (j=0;j<output_list_dim;++j)
				{
					snprintf(object_name,OPH_MAX_STRING_SIZE,"%s/1/%d",oph_web_server,j+1);
					if (strcasecmp(output_list[j],object_name))
					{
						pmesg(LOG_ERROR, __FILE__,__LINE__, "Bad object '%s' returned from the function\n",output_list[j]);
						return 1;
					}
				}
			break;

			case 9:
				if (output_list || output_list_dim)
				{
					pmesg(LOG_ERROR, __FILE__,__LINE__, "Object list is not empty: it contains %d objects\n",output_list_dim);
					return 1;
				}
			break;

			case 16:
				if (output_list || output_list_dim)
				{
					pmesg(LOG_ERROR, __FILE__,__LINE__, "Object list is not empty: it contains %d objects\n",output_list_dim);
					return 1;
				}
				if (wf->tasks[0].light_tasks_num != 3)
				{
					pmesg(LOG_ERROR, __FILE__,__LINE__, "Bad number of objects returned from the function: %d\n",output_list_dim);
					return 1;
				}
				if (!wf->tasks[0].light_tasks)
				{
					pmesg(LOG_ERROR, __FILE__,__LINE__, "Bad object list returned from the function\n");
					return 1;
				}
				for (j=0;j<wf->tasks[0].light_tasks_num;++j)
				{
					if (!wf->tasks[0].light_tasks[j].arguments_keys || !wf->tasks[0].light_tasks[j].arguments_values)
					{
						pmesg(LOG_ERROR, __FILE__,__LINE__, "Bad arguments in object %d returned from the function\n", j);
						return 1;
					}
					if (strcasecmp(wf->tasks[0].light_tasks[j].arguments_keys[0],"cube"))
					{
						pmesg(LOG_ERROR, __FILE__,__LINE__, "Bad object 'cube' returned from the function\n");
						return 1;
					}
					snprintf(object_name,OPH_MAX_STRING_SIZE,"%d",2*j+1);
					if (strcasecmp(wf->tasks[0].light_tasks[j].arguments_values[0],object_name))
					{
						pmesg(LOG_ERROR, __FILE__,__LINE__, "Bad object '%s' returned from the function\n",wf->tasks[0].light_tasks[j].arguments_values[0]);
						return 1;
					}
				}
			break;

			case 17:
				if (output_list || output_list_dim)
				{
					pmesg(LOG_ERROR, __FILE__,__LINE__, "Object list is not empty: it contains %d objects\n",output_list_dim);
					return 1;
				}
				if (wf->tasks[0].light_tasks_num)
				{
					pmesg(LOG_ERROR, __FILE__,__LINE__, "Bad number of objects returned from the function: %d\n",output_list_dim);
					return 1;
				}
				if (wf->tasks[0].light_tasks)
				{
					pmesg(LOG_ERROR, __FILE__,__LINE__, "Bad object list returned from the function\n");
					return 1;
				}
			break;

			default:
				if (output_list || output_list_dim)
				{
					pmesg(LOG_ERROR, __FILE__,__LINE__, "Object list is not empty: it contains %d objects\n",output_list_dim);
					return 1;
				}
				if (wf->tasks[0].light_tasks_num != 3)
				{
					pmesg(LOG_ERROR, __FILE__,__LINE__, "Bad number of objects returned from the function: %d\n",wf->tasks[0].light_tasks_num);
					return 1;
				}
				if (!wf->tasks[0].light_tasks)
				{
					pmesg(LOG_ERROR, __FILE__,__LINE__, "Bad object list returned from the function\n");
					return 1;
				}
				for (j=0;j<wf->tasks[0].light_tasks_num;++j)
				{
					if (wf->tasks[0].light_tasks[j].arguments_num != 2)
					{
						pmesg(LOG_ERROR, __FILE__,__LINE__, "Bad number of arguments of object %d returned from the function: %d\n", j, wf->tasks[0].light_tasks[j].arguments_num);
						return 1;
					}
					if (!wf->tasks[0].light_tasks[j].arguments_keys || !wf->tasks[0].light_tasks[j].arguments_values)
					{
						pmesg(LOG_ERROR, __FILE__,__LINE__, "Bad arguments in object %d returned from the function\n", j);
						return 1;
					}
					if (strcasecmp(wf->tasks[0].light_tasks[j].arguments_keys[0],"cube"))
					{
						pmesg(LOG_ERROR, __FILE__,__LINE__, "Bad object 'cube' returned from the function\n");
						return 1;
					}
					pmesg(LOG_DEBUG, __FILE__,__LINE__, "Argument value for %d: %s\n",j,wf->tasks[0].light_tasks[j].arguments_values[0]);
					snprintf(object_name,OPH_MAX_STRING_SIZE,"%s/1/%d",oph_web_server,j+1);
					if (strcasecmp(wf->tasks[0].light_tasks[j].arguments_values[0],object_name))
					{
						pmesg(LOG_ERROR, __FILE__,__LINE__, "Bad object '%s' returned from the function\n",wf->tasks[0].light_tasks[j].arguments_values[0]);
						return 1;
					}
				}
		}

		for (j=0;j<output_list_dim;++j) if (output_list[j]) free(output_list[j]);
		if (output_list) free(output_list);
		output_list = NULL;
		if (query) free(query);
		query = NULL;
	}
	else if (!strcmp(operator,"oph_massive2"))
	{
		int filter_num = 12;
		char *filter[] = {
			"[testdata/*]",
			"[testdata/*.test]",
			"[testdata/other/*]",
			"[testdata/other/*.tst]",
			"[path=testdata;recursive=no]",
			"[path=testdata;recursive=yes]",
			"[path=testdata/other;recursive=no]",
			"[path=testdata/other;recursive=yes]",
			"[path=testdata;file=*1*]",
			"[path=testdata;file=*1*;recursive=yes]",
			"[path=testdata;file=*12*;recursive=yes]",
			"[path=testdata/other;file=*2*te*;recursive=yes]"
		};

		// Tasks
		wf->tasks_num = 1;
		wf->residual_tasks_num = 1;
		wf->tasks = (oph_workflow_task*)calloc(1+wf->tasks_num,sizeof(oph_workflow_task));

		// MASSIVE
		wf->tasks[0].idjob = 2;
		wf->tasks[0].markerid = 2;
		wf->tasks[0].status = OPH_ODB_STATUS_PENDING;
		wf->tasks[0].name = strdup("MASSIVE");
		wf->tasks[0].operator = strdup("oph_massive");
		wf->tasks[0].role = oph_code_role("read");
		wf->tasks[0].ncores = wf->ncores;
		wf->tasks[0].arguments_num = 2;
		wf->tasks[0].arguments_keys = (char**)calloc(wf->tasks[0].arguments_num,sizeof(char*));
			wf->tasks[0].arguments_keys[0] = strdup("src_path");
			wf->tasks[0].arguments_keys[1] = strdup("cwd");
		wf->tasks[0].arguments_values = (char**)calloc(wf->tasks[0].arguments_num,sizeof(char*));
			wf->tasks[0].arguments_values[0] = strdup(filter[option < filter_num ? option : 0]);
			wf->tasks[0].arguments_values[1] = strdup(wf->cwd);
		wf->tasks[0].run = 1;

		ophidiadb oDB;
		oph_odb_initialize_ophidiadb(&oDB);

		char** output_list = NULL;
		int res, j, output_list_dim = 0;

		switch (option)
		{

			default:
				res = oph_check_for_massive_operation('T', 0, wf, 0, &oDB, &output_list, &output_list_dim, NULL);
		}

		switch (option)
		{

			default:
				if (res)
				{
					pmesg(LOG_ERROR, __FILE__,__LINE__, "Return code: %d\n",res);
					return 1;
				}
		}

		if (option < filter_num) switch (option)
		{
			case 2:
			case 6:
			case 7:
			case 10:
				if (output_list || output_list_dim)
				{
					pmesg(LOG_ERROR, __FILE__,__LINE__, "Object list is not empty: it contains %d objects\n",output_list_dim);
					return 1;
				}
				if (wf->tasks[0].light_tasks_num != 4)
				{
					pmesg(LOG_ERROR, __FILE__,__LINE__, "Bad number of objects returned from the function: %d\n",wf->tasks[0].light_tasks_num);
					return 1;
				}
				if (!wf->tasks[0].light_tasks)
				{
					pmesg(LOG_ERROR, __FILE__,__LINE__, "Bad object list returned from the function\n");
					return 1;
				}
				for (j=0;j<wf->tasks[0].light_tasks_num;++j)
				{
					if (wf->tasks[0].light_tasks[j].arguments_num != 2)
					{
						pmesg(LOG_ERROR, __FILE__,__LINE__, "Bad number of arguments of object %d returned from the function: %d\n", j, wf->tasks[0].light_tasks[j].arguments_num);
						return 1;
					}
					if (!wf->tasks[0].light_tasks[j].arguments_keys || !wf->tasks[0].light_tasks[j].arguments_values)
					{
						pmesg(LOG_ERROR, __FILE__,__LINE__, "Bad arguments in object %d returned from the function\n", j);
						return 1;
					}
					if (strcasecmp(wf->tasks[0].light_tasks[j].arguments_keys[0],"src_path"))
					{
						pmesg(LOG_ERROR, __FILE__,__LINE__, "Bad object 'cube' returned from the function\n");
						return 1;
					}
					pmesg(LOG_DEBUG, __FILE__,__LINE__, "Argument value for %d: %s\n",j,wf->tasks[0].light_tasks[j].arguments_values[0]);
				}
			break;

			case 5:
			case 9:
				if (output_list || output_list_dim)
				{
					pmesg(LOG_ERROR, __FILE__,__LINE__, "Object list is not empty: it contains %d objects\n",output_list_dim);
					return 1;
				}
				if (wf->tasks[0].light_tasks_num != 6)
				{
					pmesg(LOG_ERROR, __FILE__,__LINE__, "Bad number of objects returned from the function: %d\n",wf->tasks[0].light_tasks_num);
					return 1;
				}
				if (!wf->tasks[0].light_tasks)
				{
					pmesg(LOG_ERROR, __FILE__,__LINE__, "Bad object list returned from the function\n");
					return 1;
				}
				for (j=0;j<wf->tasks[0].light_tasks_num;++j)
				{
					if (wf->tasks[0].light_tasks[j].arguments_num != 2)
					{
						pmesg(LOG_ERROR, __FILE__,__LINE__, "Bad number of arguments of object %d returned from the function: %d\n", j, wf->tasks[0].light_tasks[j].arguments_num);
						return 1;
					}
					if (!wf->tasks[0].light_tasks[j].arguments_keys || !wf->tasks[0].light_tasks[j].arguments_values)
					{
						pmesg(LOG_ERROR, __FILE__,__LINE__, "Bad arguments in object %d returned from the function\n", j);
						return 1;
					}
					if (strcasecmp(wf->tasks[0].light_tasks[j].arguments_keys[0],"src_path"))
					{
						pmesg(LOG_ERROR, __FILE__,__LINE__, "Bad object 'cube' returned from the function\n");
						return 1;
					}
					pmesg(LOG_DEBUG, __FILE__,__LINE__, "Argument value for %d: %s\n",j,wf->tasks[0].light_tasks[j].arguments_values[0]);
				}
			break;

			case 11:
				if (output_list || output_list_dim)
				{
					pmesg(LOG_ERROR, __FILE__,__LINE__, "Object list is not empty: it contains %d objects\n",output_list_dim);
					return 1;
				}
				if (wf->tasks[0].light_tasks_num != 1)
				{
					pmesg(LOG_ERROR, __FILE__,__LINE__, "Bad number of objects returned from the function: %d\n",wf->tasks[0].light_tasks_num);
					return 1;
				}
				if (!wf->tasks[0].light_tasks)
				{
					pmesg(LOG_ERROR, __FILE__,__LINE__, "Bad object list returned from the function\n");
					return 1;
				}
				for (j=0;j<wf->tasks[0].light_tasks_num;++j)
				{
					if (wf->tasks[0].light_tasks[j].arguments_num != 2)
					{
						pmesg(LOG_ERROR, __FILE__,__LINE__, "Bad number of arguments of object %d returned from the function: %d\n", j, wf->tasks[0].light_tasks[j].arguments_num);
						return 1;
					}
					if (!wf->tasks[0].light_tasks[j].arguments_keys || !wf->tasks[0].light_tasks[j].arguments_values)
					{
						pmesg(LOG_ERROR, __FILE__,__LINE__, "Bad arguments in object %d returned from the function\n", j);
						return 1;
					}
					if (strcasecmp(wf->tasks[0].light_tasks[j].arguments_keys[0],"src_path"))
					{
						pmesg(LOG_ERROR, __FILE__,__LINE__, "Bad object 'cube' returned from the function\n");
						return 1;
					}
					pmesg(LOG_DEBUG, __FILE__,__LINE__, "Argument value for %d: %s\n",j,wf->tasks[0].light_tasks[j].arguments_values[0]);
				}
			break;

			default:
				if (output_list || output_list_dim)
				{
					pmesg(LOG_ERROR, __FILE__,__LINE__, "Object list is not empty: it contains %d objects\n",output_list_dim);
					return 1;
				}
				if (wf->tasks[0].light_tasks_num != 2)
				{
					pmesg(LOG_ERROR, __FILE__,__LINE__, "Bad number of objects returned from the function: %d\n",wf->tasks[0].light_tasks_num);
					return 1;
				}
				if (!wf->tasks[0].light_tasks)
				{
					pmesg(LOG_ERROR, __FILE__,__LINE__, "Bad object list returned from the function\n");
					return 1;
				}
				for (j=0;j<wf->tasks[0].light_tasks_num;++j)
				{
					if (wf->tasks[0].light_tasks[j].arguments_num != 2)
					{
						pmesg(LOG_ERROR, __FILE__,__LINE__, "Bad number of arguments of object %d returned from the function: %d\n", j, wf->tasks[0].light_tasks[j].arguments_num);
						return 1;
					}
					if (!wf->tasks[0].light_tasks[j].arguments_keys || !wf->tasks[0].light_tasks[j].arguments_values)
					{
						pmesg(LOG_ERROR, __FILE__,__LINE__, "Bad arguments in object %d returned from the function\n", j);
						return 1;
					}
					if (strcasecmp(wf->tasks[0].light_tasks[j].arguments_keys[0],"src_path"))
					{
						pmesg(LOG_ERROR, __FILE__,__LINE__, "Bad object 'cube' returned from the function\n");
						return 1;
					}
					pmesg(LOG_DEBUG, __FILE__,__LINE__, "Argument value for %d: %s\n",j,wf->tasks[0].light_tasks[j].arguments_values[0]);
				}
		}

		switch (option)
		{

			case 0:
			case 1:
				for (j=0;j<wf->tasks[0].light_tasks_num;++j)
				{
					if (!strstr(wf->tasks[0].light_tasks[j].arguments_values[0],"testdata/a_123.test") && !strstr(wf->tasks[0].light_tasks[j].arguments_values[0],"testdata/a_12.test"))
					{
						pmesg(LOG_ERROR, __FILE__,__LINE__, "Bad object '%s' returned from the function\n", wf->tasks[0].light_tasks[j].arguments_values[j]);
						return 1;
					}
				}
			break;

			case 2:
			case 6:
				for (j=0;j<wf->tasks[0].light_tasks_num;++j)
				{
					if (!strstr(wf->tasks[0].light_tasks[j].arguments_values[0],"testdata/other/b_123.tst") && !strstr(wf->tasks[0].light_tasks[j].arguments_values[0],"testdata/other/b_13.test") && !strstr(wf->tasks[0].light_tasks[j].arguments_values[0],"testdata/other/b_1.tst") && !strstr(wf->tasks[0].light_tasks[j].arguments_values[0],"testdata/other/b_124.test"))
					{
						pmesg(LOG_ERROR, __FILE__,__LINE__, "Bad object '%s' returned from the function\n", wf->tasks[0].light_tasks[j].arguments_values[j]);
						return 1;
					}
				}
			break;

			case 3:
			case 4:
				for (j=0;j<wf->tasks[0].light_tasks_num;++j)
				{
					if (!strstr(wf->tasks[0].light_tasks[j].arguments_values[0],"testdata/other/b_123.tst") && strstr(wf->tasks[0].light_tasks[j].arguments_values[0],"testdata/other/b_13.test") && !strstr(wf->tasks[0].light_tasks[j].arguments_values[0],"testdata/other/b_1.tst") && strstr(wf->tasks[0].light_tasks[j].arguments_values[0],"testdata/other/b_124.test"))
					{
						pmesg(LOG_ERROR, __FILE__,__LINE__, "Bad object '%s' returned from the function\n", wf->tasks[0].light_tasks[j].arguments_values[j]);
						return 1;
					}
				}
			break;

			case 5:
				for (j=0;j<wf->tasks[0].light_tasks_num;++j)
				{
					if (!strstr(wf->tasks[0].light_tasks[j].arguments_values[0],"testdata/a_123.test") && !strstr(wf->tasks[0].light_tasks[j].arguments_values[0],"testdata/a_12.test") && !strstr(wf->tasks[0].light_tasks[j].arguments_values[0],"testdata/other/b_123.tst") && !strstr(wf->tasks[0].light_tasks[j].arguments_values[0],"testdata/other/b_13.test") && !strstr(wf->tasks[0].light_tasks[j].arguments_values[0],"testdata/other/b_1.tst") && !strstr(wf->tasks[0].light_tasks[j].arguments_values[0],"testdata/other/b_124.test"))
					{
						pmesg(LOG_ERROR, __FILE__,__LINE__, "Bad object '%s' returned from the function\n", wf->tasks[0].light_tasks[j].arguments_values[j]);
						return 1;
					}
				}

			case 10:
				for (j=0;j<wf->tasks[0].light_tasks_num;++j)
				{
					if (!strstr(wf->tasks[0].light_tasks[j].arguments_values[0],"testdata/a_123.test") && !strstr(wf->tasks[0].light_tasks[j].arguments_values[0],"testdata/a_12.test") && !strstr(wf->tasks[0].light_tasks[j].arguments_values[0],"testdata/other/b_123.tst") && strstr(wf->tasks[0].light_tasks[j].arguments_values[0],"testdata/other/b_13.test") && strstr(wf->tasks[0].light_tasks[j].arguments_values[0],"testdata/other/b_1.tst") && !strstr(wf->tasks[0].light_tasks[j].arguments_values[0],"testdata/other/b_124.test"))
					{
						pmesg(LOG_ERROR, __FILE__,__LINE__, "Bad object '%s' returned from the function\n", wf->tasks[0].light_tasks[j].arguments_values[j]);
						return 1;
					}
				}
			break;

			case 11:
				for (j=0;j<wf->tasks[0].light_tasks_num;++j)
				{
					if (strstr(wf->tasks[0].light_tasks[j].arguments_values[0],"testdata/a_123.test") && strstr(wf->tasks[0].light_tasks[j].arguments_values[0],"testdata/a_12.test") && strstr(wf->tasks[0].light_tasks[j].arguments_values[0],"testdata/other/b_123.tst") && strstr(wf->tasks[0].light_tasks[j].arguments_values[0],"testdata/other/b_13.test") && strstr(wf->tasks[0].light_tasks[j].arguments_values[0],"testdata/other/b_1.tst") && !strstr(wf->tasks[0].light_tasks[j].arguments_values[0],"testdata/other/b_124.test"))
					{
						pmesg(LOG_ERROR, __FILE__,__LINE__, "Bad object '%s' returned from the function\n", wf->tasks[0].light_tasks[j].arguments_values[j]);
						return 1;
					}
				}
			break;

			default:;
		}

		for (j=0;j<output_list_dim;++j) if (output_list[j]) free(output_list[j]);
		if (output_list) free(output_list);
		output_list = NULL;
	}

	//oph_workflow_free(wf);

	return 0;
}

int check_oph_server(int* i, int* f, int n, const char* operator, int option, int abort_on_first_error)
{
	(*i)++;
	printf("TEST %d/%d: operator '%s' option %d\n", *i, n, operator, option);
	if (_check_oph_server(operator, option))
	{
		(*f)++;
		printf("FAILED\n\n");
		if (abort_on_first_error) return 1;
	}
	else printf("PASSED\n\n");
	return 0;
}

int main(int argc, char* argv[])
{
	UNUSED(argc)
	UNUSED(argv)

#if defined(_POSIX_THREADS) || defined(_SC_THREADS)
	pthread_mutex_init(&global_flag, NULL);
	pthread_mutex_init(&libssh2_flag, NULL);
	pthread_cond_init(&termination_flag, NULL);
#endif

	int ch, msglevel = LOG_INFO, abort_on_first_error = 0;
	static char *USAGE = "\nUSAGE:\noph_server_test [-a] [-d] [-v] [-w]\n";

	fprintf(stdout,"%s",OPH_VERSION);
	fprintf(stdout,"%s",OPH_DISCLAIMER);

	set_debug_level(msglevel+10);

	while ((ch = getopt(argc, argv, "advw"))!=-1)
	{
		switch (ch)
		{
			case 'a':
				abort_on_first_error = 1;
			break;
			case 'd':
				msglevel = LOG_DEBUG;
			break;
			case 'v':
				return 0;
			break;
			case 'w':
				if (msglevel<LOG_WARNING) msglevel = LOG_WARNING;
			break;
			default:
				fprintf(stdout,"%s",USAGE);
				return 0;
		}
	}

	set_debug_level(msglevel+10);
	pmesg(LOG_INFO, __FILE__,__LINE__,"Selected log level %d\n",msglevel);

	oph_server_location = strdup("..");

	char configuration_file[OPH_MAX_STRING_SIZE];
	snprintf(configuration_file,OPH_MAX_STRING_SIZE,OPH_CONFIGURATION_FILE,getenv("PWD"));
	set_global_values(configuration_file);

	int test_mode_num = 6;
	int test_num[] = { 9, 2, 9, 6, 27, 12 };
	char* test_name[] = { "oph_if", "oph_else", "oph_for", "oph_endfor", "oph_massive", "oph_massive2" };
	int i = 0, j, k, f = 0, n = 0;
	for (j=0; j<test_mode_num; ++j) n += test_num[j];

	printf("\n");

	for (k=0; k<test_mode_num; ++k)
		for (j=0; j<test_num[k]; ++j)
			if (check_oph_server(&i,&f,n,test_name[k], j, abort_on_first_error)) return 1;

	if (f) printf("WARNING: %d TASK%s FAILED out of %d\n",f,f==1?"":"S",n);

	cleanup();
	return f;
}

