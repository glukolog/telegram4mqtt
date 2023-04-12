/*
    Libraries requied: curl, jansson, mosquitto
    For run code:
     gcc teletgram4mqtt.c -o bot_mqtt -lcurl -ljansson -lmosquitto
     ./bot_mqtt

 *      ToDo:
 *              Use webhook may be better way
 *              Realise Send files/photos

*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include <jansson.h>
#include <mosquitto.h>
#include <time.h>
#include <unistd.h>

#define BOT_TOKEN "1234458745:AAH9n_F4zKLJHNIJNsdgfdg234gnoBJbe4E"
#define MQTT_IP         "10.10.0.1"
#define MQTT_PORT       1883
#define MQTT_LOGIN      "admin"
#define MQTT_PASSWD     "pass"
#define MQTT_CLIENT_ID  "Telegram4mqtt"
#define MQTT_TOPIC_DEFAULT "#"
//#define DEBUG

int64_t allowed_id[5] = { 123453455, 0, 0, 0, 0 };
struct mosquitto *mosq;

struct msg_struct {
    int64_t offset;
    int64_t id;
    char *text;
    int64_t from_id;
    char from_name[128];
    int64_t chat_id;
    char chat_name[128];
    uint64_t date;
};
typedef struct msg_struct msg_t;

struct memory {
    char *data;
    size_t size;
} resp;

struct mqtt_text_collect_2send {
    int collect_timer;
    int flag;
    uint64_t chat_id;
    char *data;
    size_t size;
} mq_rcv = {0};

// Callback for CURL requests
static size_t cb(void *data, size_t size, size_t nmemb, void *clientp) {
  size_t realsize = size * nmemb;
  struct memory *mem = (struct memory *)clientp;

  memcpy(mem->data, data, realsize);
  mem->size = realsize;
  mem->data[mem->size] = 0;
  return realsize;
}

//CURL request to api.telegram
int TelegramReq(char *req, char *msg) {
    // TelegramReq("getMe", NULL);
    // TelegramReq("sendMessage", "{\"chat_id\": \"-960865243\", \"text\": \"Reply from the bot to your TEST sent via curl\"}");
    CURL *curl;
    CURLcode res;
    struct curl_slist *list = NULL;
    char buffer[CURL_ERROR_SIZE];
    char api_addr[1024];

    strcpy(api_addr, "https://api.telegram.org/bot"BOT_TOKEN"/");
    strcat(api_addr, req);
    printf("%s\n\r", api_addr);

    if ((curl = curl_easy_init()) != NULL) {
        curl_easy_setopt(curl, CURLOPT_URL, api_addr);
        if (msg != NULL) {
            list = curl_slist_append(list, "Content-Type: application/json");
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, list);
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, msg);
        }
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&resp);
        res = curl_easy_perform(curl);
        if(res != CURLE_OK) {
            fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
        }
        #ifdef DEBUG
        else printf("%s\r\n", resp.data);
        #endif
        curl_easy_cleanup(curl);
        if (list != NULL) curl_slist_free_all(list);
    }
    return 0;
}

//Callback for MQTT connect
void on_connect(struct mosquitto *mosq, void *obj, int rc) {
    printf("ID: %d\n", * (int *) obj);
    if(rc) {
        printf("Error with result code: %d\n", rc);
        exit(-1);
    }
    mosquitto_subscribe(mosq, NULL, MQTT_TOPIC_DEFAULT, 0);
}

//Callback for MQTT message
void on_message(struct mosquitto *mosq, void *obj, const struct mosquitto_message *msg) {
    if (mq_rcv.size + msg->payloadlen < 10240 - 100) {
        char s[1024];
        sprintf(s, "%s=%s\n", msg->topic, msg->payload);
        strcpy(mq_rcv.data + mq_rcv.size, s);
        mq_rcv.size += strlen(s);
    }
    printf("mqtt: %s:(%d) %s\n", msg->topic, msg->payloadlen, (char *) msg->payload);
}

int isAllowed(int64_t id) {
    for(int i = 0 ; i < 5 ; i++) if (allowed_id[i] == id) return 1;
    return 1;
}

//Procced new message from teleram
int MessageAction(msg_t *mm) {
    time_t tt = mm->date;
    #ifdef DEBUG
    printf("\tid=%ld\n", mm->id);
    printf("\tdate(%ld)=%s\n", mm->date, ctime(&mm->date));
    printf("\tfrom=%ld %s\n", mm->from_id, mm->from_name);
    printf("\tchat=%ld %s\n", mm->chat_id, mm->chat_name);
    printf("\ttext=%s\n", mm->text);
    #endif
    if (isAllowed(mm->id)) {
        if (strcmp(mm->text, "/menu") == 0) {
            json_t *to_send = json_pack("{s:i,s:s, s:{s:[[s,s],[s]],s:b}}","chat_id", mm->chat_id, "text", "Menu called", "reply_markup", "keyboard", "/info", "/gate", "/close", "resize_keyboard", true);
            if (to_send != NULL) {
                TelegramReq("sendMessage", json_dumps(to_send, JSON_COMPACT));
                printf("menu called\n %s\n", json_dumps(to_send, JSON_COMPACT));
                json_decref(to_send);
            }
        }
        if (strcmp(mm->text, "/close") == 0) {
            char s[128];
            snprintf(s, 128, "sendMessage?text=Menu_closed&chat_id=%ld&reply_markup={\"remove_keyboard\":true}", mm->chat_id);
            TelegramReq(s, NULL);
        }
        if (strcmp(mm->text, "/info") == 0) {
            printf("info called\n");
            mosquitto_publish(mosq, NULL, "info/refresh", 1, "0", 0, false);
            mq_rcv.collect_timer = 2;
            mq_rcv.flag = 1;
            mq_rcv.chat_id = mm->chat_id;
        }
        if (strcmp(mm->text, "/gate") == 0) {
            printf("gate called\n");
            mosquitto_publish(mosq, NULL, "main/gate", 1, "1", 0, false);
        }
        if (strncmp(mm->text, "/topic", 6) == 0) {
            char *topic, *value;
            int i = 0;
            while(mm->text[i] != ' ' && (uint8_t)mm->text[i] != 0xA0) i++; // /topic sometimes ended by 0xC2,0xA0 not 0x20 (space)
            topic = strtok(mm->text + i + 1, " ");
            value = strtok(0, " ");
            printf("MQTT publish: t=%s v=%s\n", topic, value);
            if (topic != NULL && value != NULL) mosquitto_publish(mosq, NULL, topic, strlen(value), value, 0, false);
        }
    } else {
        // you not allowed answer
    }
}

int ParseJsonGetUpdates(msg_t *out, char *json_data) {
    json_t *root, *ok, *res;
    json_error_t error;
    size_t i;

    root = json_loads(json_data, 0, &error);
    if(!root) {
        fprintf(stderr, "error: on line %d: %s\n", error.line, error.text);
        return 1;
    }
    //printf("%s.\n\r", json_dumps(root, JSON_INDENT(3)));

    if ((ok = json_object_get(root, "ok")) != NULL) {
        int okey = 0;
        if (json_is_boolean(ok)) okey = json_boolean_value(ok);
        if (!okey) {
            json_decref(root);
            return 0;
        }
    }

    if ((res = json_object_get(root, "result")) != NULL) {
        for(i = 0; i < json_array_size(res); i++) {
            json_t *msg;
            json_t *data = json_array_get(res, i);
            out->offset = json_integer_value(json_object_get(data, "update_id"));
            printf("uuid=%ld\n", out->offset);
            if ((msg = json_object_get(data, "message")) != NULL) {
                const char *str = json_string_value(json_object_get(msg, "text"));
                json_t *from = json_object_get(msg, "from");
                json_t *chat = json_object_get(msg, "chat");
                if (str != NULL) strcpy(out->text, str);
                out->id = json_integer_value(json_object_get(msg, "message_id"));
                if (from != NULL) {
                    const char *fname, *lname;
                    out->from_id = json_integer_value(json_object_get(from, "id"));
                    fname = json_string_value(json_object_get(from, "first_name"));
                    lname = json_string_value(json_object_get(from, "last_name"));
                    if (fname != NULL && lname != NULL) sprintf(out->from_name, "%s %s", fname, lname);
                }
                if (chat != NULL) {
                    const char *fname, *lname;
                    out->chat_id = json_integer_value(json_object_get(chat, "id"));
                    fname = json_string_value(json_object_get(chat, "first_name"));
                    lname = json_string_value(json_object_get(chat, "last_name"));
                    if (fname != NULL && lname != NULL) sprintf(out->chat_name, "%s %s", fname, lname);
                }
                out->date = json_integer_value(json_object_get(msg, "date"));

                #ifdef DEBUG
                printf("Message:\r\n%s.\r\n", json_dumps(msg, JSON_INDENT(3)));
                #endif
                MessageAction(out);
            } else {
                printf("Unknown request:\r\n%s.\r\n", json_dumps(data, JSON_INDENT(3)));
            }
        }
    }
    json_decref(root);
    return 0;
}

int main(void) {
    time_t ss, ts;
    int poll_timer = 10;
    char s[1024];
    msg_t bot_msg={0};
    int rc, id = 42;

    if ((resp.data = malloc(1024*1024)) == NULL) {
        fprintf(stderr, "malloc error\n");
        return 1;
    }
    if ((bot_msg.text = malloc(10*1024)) == NULL) {
        fprintf(stderr, "malloc error\n");
        return 1;
    }
    if ((mq_rcv.data = malloc(10*1024)) == NULL) {
        fprintf(stderr, "malloc error\n");
        return 1;
    }

    mosquitto_lib_init();
    mosq = mosquitto_new(MQTT_CLIENT_ID, true, &id);
    mosquitto_username_pw_set(mosq, MQTT_LOGIN, MQTT_PASSWD);
    mosquitto_connect_callback_set(mosq, on_connect);
    mosquitto_message_callback_set(mosq, on_message);
    rc = mosquitto_connect(mosq, MQTT_IP, MQTT_PORT, 60);
    if(rc != 0){
        printf("Client could not connect to broker! Error Code: %d\n", rc);
        mosquitto_destroy(mosq);
        return -1;
    }
    printf("We are now connected to the broker!\n");
    //mosquitto_publish(mosq, NULL, "test/t1", 6, "Hello", 0, false);
    mosquitto_loop_start(mosq);

    ts = time(NULL) - 20;
    while(1) {
        ss = time(NULL);
        if (ts != ss) {
            ts = ss;
            if (poll_timer) poll_timer--; else {
                sprintf(s, "getUpdates?offset=%ld", bot_msg.offset + 1);
                if (!TelegramReq(s, NULL)) {
                    ParseJsonGetUpdates(&bot_msg, resp.data);
                }
                poll_timer = 5;
            }
            if (mq_rcv.collect_timer) mq_rcv.collect_timer--; else {
                if (mq_rcv.flag) {
                    json_t *to_send = json_pack("{s:i,s:s}","chat_id", mq_rcv.chat_id, "text", mq_rcv.data);
                    TelegramReq("sendMessage", json_dumps(to_send, JSON_COMPACT));
                    json_decref(to_send);
                    mq_rcv.flag = 0;
                }
                mq_rcv.size = 0;
                mq_rcv.data[0] = 0;
            }
        }
        usleep(10000);
    }

    mosquitto_loop_stop(mosq, true);
    mosquitto_disconnect(mosq);
    mosquitto_destroy(mosq);
    mosquitto_lib_cleanup();

    free(bot_msg.text);
    free(resp.data);
    free(mq_rcv.data);
    return EXIT_SUCCESS;
}
