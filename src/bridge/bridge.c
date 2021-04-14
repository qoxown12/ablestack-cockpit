/*
 * This file is part of Cockpit.
 *
 * Copyright (C) 2013 Red Hat, Inc.
 *
 * Cockpit is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * Cockpit is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with Cockpit; If not, see <http://www.gnu.org/licenses/>.
 */
#include "config.h"

#include "cockpitconnect.h"
#include "cockpitdbusinternal.h"
#include "cockpitdbusjson.h"
#include "cockpitechochannel.h"
#include "cockpitfslist.h"
#include "cockpitfsread.h"
#include "cockpitfswatch.h"
#include "cockpitfsreplace.h"
#include "cockpithttpstream.h"
#include "cockpitinteracttransport.h"
#include "cockpitnullchannel.h"
#include "cockpitpackages.h"
#include "cockpitpacketchannel.h"
#include "cockpitpipechannel.h"
#include "cockpitinternalmetrics.h"
#include "cockpitpolkitagent.h"
#include "cockpitrouter.h"
#include "cockpitwebsocketstream.h"

#include "common/cockpitassets.h"
#include "common/cockpitchannel.h"
#include "common/cockpitfdpassing.h"
#include "common/cockpithacks-glib.h"
#include "common/cockpitjson.h"
#include "common/cockpitpipetransport.h"
#include "common/cockpitsystem.h"
#include "common/cockpittest.h"
#include "common/cockpitunixfd.h"
#include "common/cockpitwebresponse.h"

#include <sys/prctl.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pwd.h>

#include <glib-unix.h>
#include <glib/gstdio.h>
#include <gio/gunixsocketaddress.h>

#include <systemd/sd-journal.h>

/* This program is run on each managed server, with the credentials
   of the user that is logged into the Server Console.
*/

static CockpitPackages *packages = NULL;

static CockpitPayloadType payload_types[] = {
  { "dbus-json3", cockpit_dbus_json_get_type },
  { "http-stream1", cockpit_http_stream_get_type },
  { "http-stream2", cockpit_http_stream_get_type },
  { "stream", cockpit_pipe_channel_get_type },
  { "packet", cockpit_packet_channel_get_type },
  { "fsread1", cockpit_fsread_get_type },
  { "fsreplace1", cockpit_fsreplace_get_type },
  { "fswatch1", cockpit_fswatch_get_type },
  { "fslist1", cockpit_fslist_get_type },
  { "null", cockpit_null_channel_get_type },
  { "echo", cockpit_echo_channel_get_type },
  { "websocket-stream1", cockpit_web_socket_stream_get_type },
  { NULL },
};

static void
add_router_channels (CockpitRouter *router)
{
  JsonObject *match;

  match = json_object_new ();
  json_object_set_string_member (match, "payload", "metrics1");
  json_object_set_string_member (match, "source", "internal");
  cockpit_router_add_channel (router, match, cockpit_internal_metrics_get_type);
  json_object_unref (match);
}

static void
on_closed_set_flag (CockpitTransport *transport,
                    const gchar *problem,
                    gpointer user_data)
{
  gboolean *flag = user_data;
  *flag = TRUE;
}

static void
send_init_command (CockpitTransport *transport,
                   gboolean interactive)
{
  const gchar *checksum;
  JsonObject *object;
  JsonObject *block;
  GHashTable *os_release;
  gchar **names;
  GBytes *bytes;
  gint i;
  gchar *session_id;

  object = json_object_new ();
  json_object_set_string_member (object, "command", "init");
  json_object_set_int_member (object, "version", 1);

  /*
   * When in interactive mode pretend we received an init
   * message, and don't print one out.
   */
  if (interactive)
    {
      json_object_set_string_member (object, "host", "localhost");
    }
  else
    {
      checksum = cockpit_packages_get_checksum (packages);
      if (checksum)
        json_object_set_string_member (object, "checksum", checksum);

      /* This is encoded as an object to allow for future expansion */
      block = json_object_new ();
      names = cockpit_packages_get_names (packages);
      for (i = 0; names && names[i] != NULL; i++)
        json_object_set_null_member (block, names[i]);
      json_object_set_object_member (object, "packages", block);
      g_free (names);

      os_release = cockpit_system_load_os_release ();
      if (os_release)
        {
          block = cockpit_json_from_hash_table (os_release,
                                                cockpit_system_os_release_fields ());
          if (block)
            json_object_set_object_member (object, "os-release", block);
          g_hash_table_unref (os_release);
        }

      session_id = secure_getenv ("XDG_SESSION_ID");
      if (session_id)
        json_object_set_string_member (object, "session-id", session_id);

      block = json_object_new ();
      json_object_set_boolean_member (block, "explicit-superuser", TRUE);
      json_object_set_object_member (object, "capabilities", block);
    }

  bytes = cockpit_json_write_bytes (object);
  json_object_unref (object);

  if (interactive)
    cockpit_transport_emit_recv (transport, NULL, bytes);
  else
    cockpit_transport_send (transport, NULL, bytes);
  g_bytes_unref (bytes);
}

static void
setup_dbus_daemon (gpointer addrfd)
{
  g_unsetenv ("G_DEBUG");
  cockpit_unix_fd_close_all (3, GPOINTER_TO_INT (addrfd));
}

static GPid
start_dbus_daemon (void)
{
  GError *error = NULL;
  GString *address = NULL;
  gchar *line;
  gsize len;
  gssize ret;
  GPid pid = 0;
  gchar *print_address = NULL;
  int addrfd[2] = { -1, -1 };
  GSpawnFlags flags;

  gchar *dbus_argv[] = {
      "dbus-daemon",
      "--print-address=X",
      "--session",
      NULL
  };

  if (pipe (addrfd))
    {
      g_warning ("pipe failed to allocate fds: %m");
      goto out;
    }

  print_address = g_strdup_printf ("--print-address=%d", addrfd[1]);
  dbus_argv[1] = print_address;

  /* The DBus daemon produces useless messages on stderr mixed in */
  flags = G_SPAWN_LEAVE_DESCRIPTORS_OPEN | G_SPAWN_SEARCH_PATH |
          G_SPAWN_STDERR_TO_DEV_NULL | G_SPAWN_STDOUT_TO_DEV_NULL;

  g_spawn_async_with_pipes (NULL, dbus_argv, NULL, flags,
                            setup_dbus_daemon, GINT_TO_POINTER (addrfd[1]),
                            &pid, NULL, NULL, NULL, &error);

  close (addrfd[1]);

  if (error != NULL)
    {
      if (g_error_matches (error, G_SPAWN_ERROR, G_SPAWN_ERROR_NOENT))
        g_debug ("couldn't start %s: %s", dbus_argv[0], error->message);
      else
        g_message ("couldn't start %s: %s", dbus_argv[0], error->message);
      g_error_free (error);
      pid = 0;
      goto out;
    }

  g_debug ("launched %s", dbus_argv[0]);

  address = g_string_new ("");
  for (;;)
    {
      len = address->len;
      g_string_set_size (address, len + 256);
      ret = read (addrfd[0], address->str + len, 256);
      if (ret < 0)
        {
          g_string_set_size (address, len);
          if (errno != EAGAIN && errno != EINTR)
            {
              g_warning ("couldn't read address from dbus-daemon: %s", g_strerror (errno));
              goto out;
            }
        }
      else if (ret == 0)
        {
          g_string_set_size (address, len);
          break;
        }
      else
        {
          g_string_set_size (address, len + ret);
          line = strchr (address->str, '\n');
          if (line != NULL)
            {
              *line = '\0';
              break;
            }
        }
    }

  if (address->str[0] == '\0')
    {
      g_message ("dbus-daemon didn't send us a dbus address; not installed?");
    }
  else
    {
      g_setenv ("DBUS_SESSION_BUS_ADDRESS", address->str, TRUE);
      g_debug ("session bus address: %s", address->str);
    }

out:
  if (addrfd[0] >= 0)
    close (addrfd[0]);
  if (address)
    g_string_free (address, TRUE);
  g_free (print_address);
  return pid;
}

static void
setup_ssh_agent (gpointer addrfd)
{
  g_unsetenv ("G_DEBUG");
  prctl (PR_SET_PDEATHSIG, SIGTERM);
  cockpit_unix_fd_close_all (3, GPOINTER_TO_INT (addrfd));
}

static GPid
start_ssh_agent (void)
{
  GError *error = NULL;
  GPid pid = 0;
  gint fd = -1;
  gint status = -1;

  gchar *pid_line = NULL;
  gchar *agent_output = NULL;
  gchar *agent_error = NULL;
  gchar *bind_address = g_strdup_printf ("%s/ssh-agent.XXXXXX", g_get_user_runtime_dir ());

  gchar *agent_argv[] = {
      "ssh-agent",
      "-a",
      bind_address,
      NULL
  };

  fd = g_mkstemp (bind_address);
  if (fd < 0)
    {
      g_warning ("couldn't create temporary socket file: %s", g_strerror (errno));
      goto out;
    }
  if (g_unlink (bind_address) < 0)
    {
      g_warning ("couldn't remove temporary socket file: %s", g_strerror (errno));
      goto out;
    }

  if (!g_spawn_sync (NULL, agent_argv, NULL,
                     G_SPAWN_SEARCH_PATH, setup_ssh_agent,
                     GINT_TO_POINTER (-1),
                     &agent_output, &agent_error,
                     &status, &error))
    {
      if (g_error_matches (error, G_SPAWN_ERROR, G_SPAWN_ERROR_NOENT))
        g_debug ("couldn't start %s: %s", agent_argv[0], error->message);
      else
        g_warning ("couldn't start %s: %s", agent_argv[0], error->message);
      goto out;
    }

  if (!g_spawn_check_exit_status (status, &error))
    {
      g_warning ("couldn't start %s: %s: %s", agent_argv[0],
                 error->message, agent_error);
      goto out;
    }

  pid_line = strstr (agent_output, "SSH_AGENT_PID=");
  if (pid_line)
    {
      if (sscanf (pid_line, "SSH_AGENT_PID=%d;", &pid) != 1)
        {
            g_warning ("couldn't find pid in %s", pid_line);
            goto out;
        }
    }

  if (pid < 1)
    {
      g_warning ("couldn't get agent pid from ssh-agent output: %s", agent_output);
      goto out;
    }

  g_debug ("launched %s", agent_argv[0]);
  g_setenv ("SSH_AUTH_SOCK", bind_address, TRUE);

out:
  g_clear_error (&error);
  if (fd >= 0)
    close (fd);
  g_free (bind_address);
  g_free (agent_error);
  g_free (agent_output);
  return pid;
}

static gboolean
have_env (const gchar *name)
{
  const gchar *env = g_getenv (name);
  return env && env[0];
}

static gboolean
on_signal_done (gpointer data)
{
  gboolean *closed = data;
  *closed = TRUE;
  return TRUE;
}

static struct passwd *
getpwuid_a (uid_t uid)
{
  int err;
  long bufsize = sysconf (_SC_GETPW_R_SIZE_MAX);
  struct passwd *ret = NULL;
  struct passwd *buf;

  if (bufsize <= 0)
    bufsize = 8192;

  buf = g_malloc (sizeof(struct passwd) + bufsize);
  err = getpwuid_r (uid, buf, (char *)(buf + 1), bufsize, &ret);

  if (ret == NULL)
    {
      free (buf);
      if (err == 0)
        err = ENOENT;
      errno = err;
    }

  return ret;
}

static void
update_router (CockpitRouter *router,
               gboolean privileged_peer)
{
  if (!privileged_peer)
    {
      GList *bridges = cockpit_packages_get_bridges (packages);
      cockpit_router_set_bridges (router, bridges);
      g_list_free (bridges);
    }
}

static CockpitRouter *
setup_router (CockpitTransport *transport,
              gboolean privileged_peer)
{
  CockpitRouter *router = NULL;

  packages = cockpit_packages_new ();

  router = cockpit_router_new (transport, payload_types, NULL);
  add_router_channels (router);

  /* This has to happen after add_router_channels as the
   * packages based bridges should have priority.
   */
  update_router (router, privileged_peer);

  return router;
}

struct CallUpdateRouterData {
  CockpitRouter *router;
  gboolean privileged_peer;
};

static void
call_update_router (gconstpointer user_data)
{
  const struct CallUpdateRouterData *data = user_data;
  update_router (data->router, data->privileged_peer);
}

static int
run_bridge (const gchar *interactive,
            gboolean privileged_peer)
{
  CockpitTransport *transport;
  CockpitRouter *router;
  gboolean terminated = FALSE;
  gboolean interrupted = FALSE;
  gboolean closed = FALSE;
  const gchar *directory;
  struct passwd *pwd;
  GPid daemon_pid = 0;
  GPid agent_pid = 0;
  guint sig_term;
  guint sig_int;
  uid_t uid;
  struct CallUpdateRouterData call_update_router_data;

  cockpit_hacks_redirect_gdebug_to_stderr ();

  /* Always set environment variables early */
  uid = geteuid();
  pwd = getpwuid_a (uid);
  if (pwd == NULL)
    {
      g_message ("couldn't get user info: %s", g_strerror (errno));
    }
  else
    {
      g_setenv ("USER", pwd->pw_name, TRUE);
      g_setenv ("HOME", pwd->pw_dir, TRUE);
      g_setenv ("SHELL", pwd->pw_shell, TRUE);
    }

  /* Set a path if nothing is set */
  g_setenv ("PATH", "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin", 0);

  /*
   * The bridge always runs from within $XDG_RUNTIME_DIR
   * This makes it easy to create user sockets and/or files.
   */
  if (!privileged_peer)
    {
      directory = g_get_user_runtime_dir ();
      if (g_mkdir_with_parents (directory, 0700) < 0)
        g_warning ("couldn't create runtime dir: %s: %s", directory, g_strerror (errno));
      else if (g_chdir (directory) < 0)
        g_warning ("couldn't change to runtime dir: %s: %s", directory, g_strerror (errno));
    }

  /* Reset the umask, typically this is done in .bashrc for a login shell */
  umask (022);

  /* Start daemons if necessary */
  if (!interactive && !privileged_peer)
    {
      if (!have_env ("DBUS_SESSION_BUS_ADDRESS"))
        daemon_pid = start_dbus_daemon ();
      if (!have_env ("SSH_AUTH_SOCK"))
        agent_pid = start_ssh_agent ();
    }

  sig_term = g_unix_signal_add (SIGTERM, on_signal_done, &terminated);
  sig_int = g_unix_signal_add (SIGINT, on_signal_done, &interrupted);

  cockpit_dbus_internal_startup (interactive != NULL);

  if (interactive)
    {
      /* Allow skipping the init message when interactive */
      transport = cockpit_interact_transport_new (0, 1, interactive);
    }
  else
    {
      transport = cockpit_pipe_transport_new_fds ("stdio", 0, 1);
    }

  router = setup_router (transport, privileged_peer);

#ifdef WITH_POLKIT
  gpointer polkit_agent = NULL;
  if (uid != 0)
    {
      if (!interactive)
        polkit_agent = cockpit_polkit_agent_register (transport, router, NULL);
    }
#endif

  g_resources_register (cockpitassets_get_resource ());
  cockpit_web_failure_resource = "/org/cockpit-project/Cockpit/fail.html";

  cockpit_dbus_user_startup (pwd);
  cockpit_dbus_process_startup ();
  cockpit_dbus_machines_startup ();
  cockpit_dbus_config_startup ();
  cockpit_packages_dbus_startup (packages);
  cockpit_dbus_login_messages_startup ();
  cockpit_router_dbus_startup (router);

  call_update_router_data.router = router;
  call_update_router_data.privileged_peer = privileged_peer;
  cockpit_packages_on_change (packages, call_update_router, &call_update_router_data);

  g_free (pwd);
  pwd = NULL;

  g_signal_connect (transport, "closed", G_CALLBACK (on_closed_set_flag), &closed);
  send_init_command (transport, interactive ? TRUE : FALSE);

  while (!terminated && !closed && !interrupted)
    g_main_context_iteration (NULL, TRUE);

#ifdef WITH_POLKIT
  if (polkit_agent)
    cockpit_polkit_agent_unregister (polkit_agent);
#endif

  g_object_unref (router);
  g_object_unref (transport);

  cockpit_packages_on_change (packages, NULL, NULL);

  cockpit_dbus_machines_cleanup ();
  cockpit_dbus_internal_cleanup ();

  if (daemon_pid)
    kill (daemon_pid, SIGTERM);
  if (agent_pid)
    kill (agent_pid, SIGTERM);

  g_source_remove (sig_term);
  g_source_remove (sig_int);

  /* HACK: Valgrind contains a bug that causes it to hang when the main
   * thread exits quickly in response to a signal received by a handler
   * in another thread, when that other thread is waiting in a syscall.
   * Avoid that situation by delaying our exit here, but only under
   * Valgrind.
   *
   * Remove this when https://bugs.kde.org/show_bug.cgi?id=409367 is
   * fixed and widely distributed.
   */
  if (strstr (g_getenv ("LD_PRELOAD") ?: "", "valgrind") != NULL)
    g_usleep (5 * G_TIME_SPAN_SECOND);

  /* So the caller gets the right signal */
  if (terminated)
    raise (SIGTERM);

  return 0;
}

static void
print_rules (gboolean opt_privileged)
{
  CockpitRouter *router = NULL;
  CockpitTransport *transport = cockpit_interact_transport_new (0, 1, "--");

  router = setup_router (transport, opt_privileged);

  cockpit_router_dump_rules (router);

  g_object_unref (router);
  g_object_unref (transport);
}

static void
print_version (void)
{
  gint i, offset, len;

  g_print ("Version: %s\n", PACKAGE_VERSION);
  g_print ("Protocol: 1\n");

  g_print ("Payloads: ");
  offset = 10;
  for (i = 0; payload_types[i].name != NULL; i++)
    {
      len = strlen (payload_types[i].name);
      if (offset + len > 70)
        {
          g_print ("\n");
          offset = 0;
        }

      if (offset == 0)
        {
          g_print ("    ");
          offset = 4;
        };

      g_print ("%s ", payload_types[i].name);
      offset += len + 1;
    }
  g_print ("\n");

  g_print ("Authorization: crypt1\n");
}

int
main (int argc,
      char **argv)
{
  GOptionContext *context;
  GError *error = NULL;
  int ret;

  static gboolean opt_packages = FALSE;
  static gboolean opt_rules = FALSE;
  static gboolean opt_privileged = FALSE;
  static gboolean opt_version = FALSE;
  static gchar *opt_interactive = NULL;

  static GOptionEntry entries[] = {
    { "interact", 0, 0, G_OPTION_ARG_STRING, &opt_interactive, "Interact with the raw protocol", "boundary" },
    { "privileged", 0, 0, G_OPTION_ARG_NONE, &opt_privileged, "Privileged copy of bridge", NULL },
    { "packages", 0, 0, G_OPTION_ARG_NONE, &opt_packages, "Show Cockpit package information", NULL },
    { "rules", 0, 0, G_OPTION_ARG_NONE, &opt_rules, "Show Cockpit bridge rules", NULL },
    { "version", 0, 0, G_OPTION_ARG_NONE, &opt_version, "Show Cockpit version information", NULL },
    { NULL }
  };

  signal (SIGPIPE, SIG_IGN);

  /* Debugging issues during testing */
  signal (SIGABRT, cockpit_test_signal_backtrace);
  signal (SIGSEGV, cockpit_test_signal_backtrace);

  if (g_strv_contains ((const gchar **) argv, "--privileged"))
    {
      /* We are being spawned, under sudo or pkexec, by the user's copy
       * of the bridge.  In that case, the first thing that will happen,
       * if we receive our stderr via the socket that is our stdin.
       */
      const char *msg = "\n{\"command\": \"send-stderr\"}";
      g_print ("%zu\n%s", strlen (msg), msg);

      int parent_stderr_fd;
      int r = cockpit_socket_receive_fd (STDIN_FILENO, &parent_stderr_fd);
      if (r == 0)
        {
          /* on EOF, just silently exit */
          return 0;
        }
      else if (r == -1)
        {
          g_printerr ("cockpit-bridge: recvmsg(stdin) failed: %m\n");
          return 1;
        }
      else if (parent_stderr_fd == -1)
        {
          g_printerr ("cockpit-bridge: message from stdin contains no fd\n");
          return 1;
        }
      else
        {
          r = dup2 (parent_stderr_fd, STDERR_FILENO);
          g_assert (r == STDERR_FILENO); /* that should really never fail */
          close (parent_stderr_fd);
        }
    }
  else if (g_getenv ("SSH_CONNECTION") && !g_log_writer_is_journald (2) && ! isatty(2))
    {
      /* In case we are run via sshd and we have journald, make sure all
       * logging output ends up in the journal on *this* machine, not sent
       * back to the client.
       */
      int fd = sd_journal_stream_fd ("cockpit/ssh", LOG_WARNING, 0);

      /* If it didn't work, then there's no journal.  That's OK: we'll
       * just send the output back to the client after all.
       *
       * If it did work, rename the fd to 2 (stderr).
       */
      if (fd >= 0)
        {
          int r = dup2 (fd, 2);
          g_assert (r == 2); /* that should really never fail */
          close (fd);
        }
    }

  /*
   * We have to tell GLib about an alternate default location for XDG_DATA_DIRS
   * if we've been compiled with a different prefix. GLib caches that, so need
   * to do this very early.
   */
  if (!g_getenv ("XDG_DATA_DIRS") && !g_str_equal (DATADIR, "/usr/share"))
    g_setenv ("XDG_DATA_DIRS", DATADIR, TRUE);

  g_setenv ("LANG", "C.UTF-8", FALSE);
  g_setenv ("GSETTINGS_BACKEND", "memory", TRUE);

  context = g_option_context_new (NULL);
  g_option_context_add_main_entries (context, entries, NULL);
  g_option_context_set_description (context,
                                    "cockpit-bridge is run automatically inside of a Cockpit session. When\n"
                                    "run from the command line one of the options above must be specified.\n");

  g_option_context_parse (context, &argc, &argv, &error);
  g_option_context_free (context);

  if (error)
    {
      g_printerr ("cockpit-bridge: %s\n", error->message);
      g_error_free (error);
      return 1;
    }

  if (opt_packages)
    {
      cockpit_packages_dump ();
      return 0;
    }
  else if (opt_rules)
    {
      print_rules (opt_privileged);
      return 0;
    }
  else if (opt_version)
    {
      print_version ();
      return 0;
    }

  if (!opt_interactive && isatty (1))
    {
      g_printerr ("cockpit-bridge: no option specified\n");
      return 2;
    }

  ret = run_bridge (opt_interactive, opt_privileged);

  if (packages)
    cockpit_packages_free (packages);

  g_free (opt_interactive);
  return ret;
}
