#include "server.h"

#define BUF_SIZE 1024

int
send_initial_message(struct lws *wsi) {
    unsigned char message[LWS_PRE + 256];
    unsigned char *p = &message[LWS_PRE];
    int n;

    char hostname[128];
    gethostname(hostname, sizeof(hostname) - 1);
    hostname[127] = '\0';

    // window title
    n = snprintf((char *) p, 256 - LWS_PRE, "%c%s (%s)", SET_WINDOW_TITLE, server->argv[0], hostname);
    if (lws_write(wsi, p, (size_t) n, LWS_WRITE_TEXT) < n) {
        return -1;
    }
    // reconnect time
    n = snprintf((char *) p, 12, "%c%d", SET_RECONNECT, server->reconnect);
    if (lws_write(wsi, p, (size_t) n, LWS_WRITE_TEXT) < n) {
        return -1;
    }
    // client preferences
    n = snprintf((char *) p, 256, "%c%s", SET_PREFERENCES, server->client_opt);
    if (lws_write(wsi, p, (size_t) n, LWS_WRITE_TEXT) < n) {
        return -1;
    }
    return 0;
}

bool
parse_window_size(const char *json, struct winsize *size) {
    int columns, rows;
    json_object *obj = json_tokener_parse(json);
    struct json_object *o = NULL;

    if (!json_object_object_get_ex(obj, "columns", &o)) {
        lwsl_err("columns field not exists, json: %s\n", json);
        return false;
    }
    columns = json_object_get_int(o);
    if (!json_object_object_get_ex(obj, "rows", &o)) {
        lwsl_err("rows field not exists, json: %s\n", json);
        return false;
    }
    rows = json_object_get_int(o);
    json_object_put(obj);

    memset(size, 0, sizeof(struct winsize));
    size->ws_col = (unsigned short) columns;
    size->ws_row = (unsigned short) rows;

    return true;
}

bool
check_host_origin(struct lws *wsi) {
    int origin_length = lws_hdr_total_length(wsi, WSI_TOKEN_ORIGIN);
    char buf[origin_length + 1];
    memset(buf, 0, sizeof(buf));
    int len = lws_hdr_copy(wsi, buf, sizeof(buf), WSI_TOKEN_ORIGIN);
    if (len > 0) {
        const char *prot, *address, *path;
        int port;
        if (lws_parse_uri(buf, &prot, &address, &port, &path))
            return false;
        snprintf(buf, origin_length, "%s:%d", address, port);
        int host_length = lws_hdr_total_length(wsi, WSI_TOKEN_HOST);
        if (host_length != strlen(buf))
            return false;
        char host_buf[host_length + 1];
        memset(host_buf, 0, sizeof(host_buf));
        len = lws_hdr_copy(wsi, host_buf, sizeof(host_buf), WSI_TOKEN_HOST);
        return len > 0 && strcasecmp(buf, host_buf) == 0;
    }
    return false;
}

void
tty_client_remove(struct tty_client *client) {
    pthread_mutex_lock(&server->lock);
    struct tty_client *iterator;
    LIST_FOREACH(iterator, &server->clients, list) {
        if (iterator == client) {
            LIST_REMOVE(iterator, list);
            server->client_count--;
            break;
        }
    }
    pthread_mutex_unlock(&server->lock);
}

void
tty_client_destroy(struct tty_client *client) {
    if (!client->running || client->pid <= 0)
        return;
    client->running = false;

    // kill process and free resource
    lwsl_notice("tty_client_destroy: sending  %s (%d) to process %d\n", server->sig_name, server->sig_code, client->pid);
    if (kill(client->pid, server->sig_code) != 0) {
        lwsl_err("kill: %d, errno: %d (%s)\n", client->pid, errno, strerror(errno));
    }
    int status;
    while (waitpid(client->pid, &status, 0) == -1 && errno == EINTR)
        ;
    lwsl_notice("process exited with code %d, pid: %d\n", status, client->pid);
    close(client->pty);

    // free the buffer
    if (client->buffer != NULL)
        free(client->buffer);

    // remove from client list
    tty_client_remove(client);
}

void *
thread_run_command(void *args) {
    struct tty_client *client;
    int pty;
    int bytes;
    char buf[BUF_SIZE];
    fd_set des_set;

    client = (struct tty_client *) args;
    pid_t pid = forkpty(&pty, NULL, NULL, NULL);

    switch (pid) {
        case -1: /* error */
            lwsl_err("forkpty, error: %d (%s)\n", errno, strerror(errno));
            break;
        case 0: /* child */
            if (setenv("TERM", "xterm-256color", true) < 0) {
                perror("setenv");
                exit(1);
            }
            int e = -1;
            if ( 0 == access(server->argv[0], R_OK | X_OK) ) {
                e = execvp(server->argv[0], server->argv);
            } else {
                char *argp[] = {"sh", "-c", NULL, NULL};
                argp[2] = server->argv[0];
                e = execv("/bin/sh", argp);
            } 
            if (e < 0) {
                perror("execv?");
                exit(1);
            }
            break;
        default: /* parent */
            lwsl_notice("started process, pid: %d\n", pid);
            client->pid = pid;
            client->pty = pty;
            client->running = true;
            if (client->size.ws_row > 0 && client->size.ws_col > 0)
                ioctl(client->pty, TIOCSWINSZ, &client->size);

            while (client->running) {
                FD_ZERO (&des_set);
                FD_SET (pty, &des_set);

                if (select(pty + 1, &des_set, NULL, NULL, NULL) < 0)
                    break;

                if (FD_ISSET (pty, &des_set)) {
                    memset(buf, 0, BUF_SIZE);
                    bytes = (int) read(pty, buf, BUF_SIZE);
                    struct pty_data *frame = (struct pty_data *) xmalloc(sizeof(struct pty_data));
                    frame->len = bytes;
                    if (bytes > 0) {
                        frame->data = xmalloc((size_t) bytes);
                        memcpy(frame->data, buf, bytes);
                    }
                    pthread_mutex_lock(&client->lock);
                    STAILQ_INSERT_TAIL(&client->queue, frame, list);
                    pthread_mutex_unlock(&client->lock);
                }
            }
            break;
    }

    return 0;
}

int
callback_tty(struct lws *wsi, enum lws_callback_reasons reason,
             void *user, void *in, size_t len) {
    struct tty_client *client = (struct tty_client *) user;
    char buf[256];

    switch (reason) {
        case LWS_CALLBACK_FILTER_PROTOCOL_CONNECTION:
            if (server->once && server->client_count > 0) {
                lwsl_warn("refuse to serve WS client due to the --once option.\n");
                return 1;
            }
            if (server->max_clients > 0 && server->client_count == server->max_clients) {
                lwsl_warn("refuse to serve WS client due to the --max-clients option.\n");
                return 1;
            }
            if (lws_hdr_copy(wsi, buf, sizeof(buf), WSI_TOKEN_GET_URI) <= 0 || strcmp(buf, WS_PATH)) {
                lwsl_warn("refuse to serve WS client for illegal ws path: %s\n", buf);
                return 1;
            }

            if (server->check_origin && !check_host_origin(wsi)) {
                lwsl_warn("refuse to serve WS client from different origin due to the --check-origin option.\n");
                return 1;
            }
            break;

        case LWS_CALLBACK_ESTABLISHED:
            client->running = false;
            client->initialized = false;
            client->authenticated = false;
            client->wsi = wsi;
            client->buffer = NULL;
            lws_get_peer_addresses(wsi, lws_get_socket_fd(wsi),
                                   client->hostname, sizeof(client->hostname),
                                   client->address, sizeof(client->address));
            STAILQ_INIT(&client->queue);

            pthread_mutex_lock(&server->lock);
            LIST_INSERT_HEAD(&server->clients, client, list);
            server->client_count++;
            pthread_mutex_unlock(&server->lock);
            lws_hdr_copy(wsi, buf, sizeof(buf), WSI_TOKEN_GET_URI);

            lwsl_notice("WS   %s - %s (%s), clients: %d\n", buf, client->address, client->hostname, server->client_count);
            break;

        case LWS_CALLBACK_SERVER_WRITEABLE:
            if (!client->initialized) {
                if (send_initial_message(wsi) < 0) {
                    lwsl_err("tty_client_remove: failed\n");
                    tty_client_remove(client);
                    lws_close_reason(wsi, LWS_CLOSE_STATUS_UNEXPECTED_CONDITION, NULL, 0);
                    return -1;
                }
                client->initialized = true;
                break;
            }

            pthread_mutex_lock(&client->lock);
            while (!STAILQ_EMPTY(&client->queue)) {
                struct pty_data *frame = STAILQ_FIRST(&client->queue);
                // read error or client exited, close connection
                if (frame->len <= 0) {
                    STAILQ_REMOVE_HEAD(&client->queue, list);
                    free(frame);
                    tty_client_remove(client);
                    lws_close_reason(wsi,
                                     frame->len == 0 ? LWS_CLOSE_STATUS_NORMAL : LWS_CLOSE_STATUS_UNEXPECTED_CONDITION,
                                     NULL, 0);
                    return -1;
                }

                char *b64_text = base64_encode((const unsigned char *) frame->data, (size_t) frame->len);
                size_t msg_len = LWS_PRE + strlen(b64_text) + 1;
                unsigned char message[msg_len];
                unsigned char *p = &message[LWS_PRE];
                size_t n = snprintf((char *) p, msg_len-LWS_PRE+1, "%c%s", OUTPUT, b64_text);

                free(b64_text);

                if (lws_write(wsi, p, n, LWS_WRITE_TEXT) < n) {
                    lwsl_err("write data to WS\n");
                    break;
                }

                STAILQ_REMOVE_HEAD(&client->queue, list);
                free(frame->data);
                free(frame);

                if (lws_partial_buffered(wsi)) {
                    lws_callback_on_writable(wsi);
                    break;
                }
            }
            pthread_mutex_unlock(&client->lock);
            break;

        case LWS_CALLBACK_RECEIVE:
            if (client->buffer == NULL) {
                client->buffer = xmalloc(len + 1);
                client->len = len;
                memcpy(client->buffer, in, len);
            } else {
                client->buffer = xrealloc(client->buffer, client->len + len + 1);
                memcpy(client->buffer + client->len, in, len);
                client->len += len;
            }
            client->buffer[client->len] = '\0';

            const char command = client->buffer[0];

            // check auth
            if (server->credential != NULL && !client->authenticated && command != JSON_DATA) {
                lwsl_warn("WS client not authenticated\n");
                return 1;
            }

            // check if there are more fragmented messages
            if (lws_remaining_packet_payload(wsi) > 0 || !lws_is_final_fragment(wsi)) {
                return 0;
            }

            switch (command) {
                case INPUT:
                    if (client->pty == 0)
                        break;
                    if (server->readonly)
                        return 0;
                    if (write(client->pty, client->buffer + 1, client->len - 1) < client->len - 1) {
                        lwsl_err("write INPUT to pty\n");
                        tty_client_remove(client);
                        lws_close_reason(wsi, LWS_CLOSE_STATUS_UNEXPECTED_CONDITION, NULL, 0);
                        return -1;
                    }
                    break;
                case PING:
                    {
                        unsigned char c = PONG;
                        if (lws_write(wsi, &c, 1, LWS_WRITE_TEXT) != 1) {
                            lwsl_err("send PONG\n");
                            tty_client_remove(client);
                            lws_close_reason(wsi, LWS_CLOSE_STATUS_UNEXPECTED_CONDITION, NULL, 0);
                            return -1;
                        }
                    }
                    break;
                case RESIZE_TERMINAL:
                    if (parse_window_size(client->buffer + 1, &client->size) && client->pty > 0) {
                        if (ioctl(client->pty, TIOCSWINSZ, &client->size) == -1) {
                            lwsl_err("ioctl TIOCSWINSZ: %d (%s)\n", errno, strerror(errno));
                        }
                    }
                    break;
                case JSON_DATA:
                    if (client->pid > 0)
                        break;
                    if (server->credential != NULL) {
                        json_object *obj = json_tokener_parse(client->buffer);
                        struct json_object *o = NULL;
                        if (json_object_object_get_ex(obj, "AuthToken", &o)) {
                            const char *token = json_object_get_string(o);
                            if (token != NULL && !strcmp(token, server->credential))
                                client->authenticated = true;
                            else
                                lwsl_warn("WS authentication failed with token: %s\n", token);
                        }
                        if (!client->authenticated) {
                            tty_client_remove(client);
                            lws_close_reason(wsi, LWS_CLOSE_STATUS_POLICY_VIOLATION, NULL, 0);
                            return -1;
                        }
                    }
                    int err = pthread_create(&client->thread, NULL, thread_run_command, client);
                    if (err != 0) {
                        lwsl_err("pthread_create return: %d\n", err);
                        return 1;
                    }
                    break;
                default:
                    lwsl_warn("unknown message type: %c\n", command);
                    lws_close_reason(wsi, LWS_CLOSE_STATUS_INVALID_PAYLOAD, NULL, 0);
                    return -1;
            }

            if (client->buffer != NULL) {
                free(client->buffer);
                client->buffer = NULL;
            }
            break;

        case LWS_CALLBACK_CLOSED:
            tty_client_destroy(client);
            lwsl_notice("WS closed from %s (%s), clients: %d\n", client->address, client->hostname, server->client_count);
            if (server->once && server->client_count == 0) {
                lwsl_notice("exiting due to the --once option.\n");
                force_exit = true;
                lws_cancel_service(context);
                exit(0);
            }
            break;

        default:
            break;
    }

    return 0;
}
