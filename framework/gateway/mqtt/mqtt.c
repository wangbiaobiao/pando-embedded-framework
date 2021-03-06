/* mqtt.c
*  Protocol: http://docs.oasis-open.org/mqtt/mqtt/v3.1.1/os/mqtt-v3.1.1-os.html
*
* Copyright (c) 2014-2015, Tuan PM <tuanpm at live dot com>
* All rights reserved.
*
* Redistribution and use in source and binary forms, with or without
* modification, are permitted provided that the following conditions are met:
*
* * Redistributions of source code must retain the above copyright notice,
* this list of conditions and the following disclaimer.
* * Redistributions in binary form must reproduce the above copyright
* notice, this list of conditions and the following disclaimer in the
* documentation and/or other materials provided with the distribution.
* * Neither the name of Redis nor the names of its contributors may be used
* to endorse or promote products derived from this software without
* specific prior written permission.
*
* THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
* AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
* IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
* ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
* LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
* CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
* SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
* INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
* CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
* ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
* POSSIBILITY OF SUCH DAMAGE.
*/

#include "mqtt_msg.h"
#include "debug.h"
#include "mqtt.h"
#include "queue.h"
#include "../../platform/include/pando_sys.h"
#include "../../platform/include/pando_types.h"
#include "../../platform/include/pando_timer.h"
#include "../../platform/include/pando_net_tcp.h"
#include "utils.h"
#include "../../protocol/common_functions.h"

#define MQTT_BUF_SIZE		1024
#define MQTT_RECONNECT_TIMEOUT 5
#define MQTT_CONNECT_TIMEOUT   20

#define MQTT_TASK_PRIO        		0
#define MQTT_TASK_QUEUE_SIZE    	1
#define MQTT_SEND_TIMOUT			5

#ifndef QUEUE_BUFFER_SIZE
#define QUEUE_BUFFER_SIZE		 	2048
#endif

void MQTT_Task(MQTT_Client * arg);


static void FUNCTION_ATTRIBUTE
deliver_publish(MQTT_Client* client, uint8_t* message, int length)
{
	mqtt_event_data_t event_data;

	event_data.topic_length = length;
	event_data.topic = mqtt_get_publish_topic(message, &event_data.topic_length);
	event_data.data_length = length;
	event_data.data = mqtt_get_publish_data(message, &event_data.data_length);

	if(client->dataCb)
		client->dataCb((uint32_t*)client, event_data.topic, event_data.topic_length, event_data.data, event_data.data_length);

}


/**
  * @brief  Client received callback function.
  * @param  arg: contain the ip link information
  * @param  pdata: received data
  * @param  len: the lenght of received data
  * @retval None
  */
void FUNCTION_ATTRIBUTE
mqtt_tcpclient_recv(void *arg, struct data_buf *buffer)
{
	INFO("TCP: data received %d bytes\r\n", buffer->length);

	uint8_t msg_type;
	uint8_t msg_qos;
	uint16_t msg_id;

	struct pando_tcp_conn *pCon = (struct pando_tcp_conn*)arg;
	MQTT_Client *client = (MQTT_Client *)pCon->reverse;

	if(buffer->length > MQTT_BUF_SIZE || buffer->length == 0)
	{
		INFO("receive length is invalid.\n");
		return;
	}

	pd_memcpy(client->mqtt_state.in_buffer + client->mqtt_state.message_length_read, buffer->data, buffer->length);
	client->mqtt_state.message_length_read += buffer->length;

READPACKET:
	if(client->mqtt_state.message_length_read == 1)
	{
		INFO("not enough data for read package length.\n!");
		return;
	}

	client->mqtt_state.message_length = mqtt_get_total_length(client->mqtt_state.in_buffer, client->mqtt_state.message_length_read);
	INFO("message length:%d\n", client->mqtt_state.message_length);

	show_package(client->mqtt_state.in_buffer, client->mqtt_state.message_length_read);
	
	if(client->mqtt_state.message_length > client->mqtt_state.message_length_read)
	{
		INFO("not enough data.\n");
		return;
	}
	
	msg_type = mqtt_get_type(client->mqtt_state.in_buffer);
	msg_qos = mqtt_get_qos(client->mqtt_state.in_buffer);
	msg_id = mqtt_get_id(client->mqtt_state.in_buffer, client->mqtt_state.message_length);
	INFO("client->connstate:%d, type:%d, Qos:%d, id:%d, message length:%d\n", client->connState, msg_type, msg_qos, \
			msg_id, client->mqtt_state.message_length);
	switch(client->connState)

	{
		case MQTT_CONNECT_SENDING:
		if(msg_type == MQTT_MSG_TYPE_CONNACK)
		{
			if(client->mqtt_state.pending_msg_type != MQTT_MSG_TYPE_CONNECT)
			{
				INFO("MQTT: Invalid packet\r\n");
				net_tcp_disconnect(client->pCon);
			}

			else
			{
				INFO("MQTT: Connected to %s:%d\r\n", client->host, client->port);
				client->connState = MQTT_DATA;
				if(client->connectedCb)
					client->connectedCb((uint32_t*)client);
			}

		}
		break;

		case MQTT_DATA:
		switch(msg_type)
		{
			case MQTT_MSG_TYPE_SUBACK:
			if(client->mqtt_state.pending_msg_type == MQTT_MSG_TYPE_SUBSCRIBE && client->mqtt_state.pending_msg_id == msg_id)
			INFO("MQTT: Subscribe successful\r\n");
			break;

			case MQTT_MSG_TYPE_UNSUBACK:
			if(client->mqtt_state.pending_msg_type == MQTT_MSG_TYPE_UNSUBSCRIBE && client->mqtt_state.pending_msg_id == msg_id)
			INFO("MQTT: UnSubscribe successful\r\n");
			break;

			case MQTT_MSG_TYPE_PUBLISH:
			if(msg_qos == 1)
			client->mqtt_state.outbound_message = mqtt_msg_puback(&client->mqtt_state.mqtt_connection, msg_id);
			else if(msg_qos == 2)
			client->mqtt_state.outbound_message = mqtt_msg_pubrec(&client->mqtt_state.mqtt_connection, msg_id);
			if(msg_qos == 1 || msg_qos == 2)
			{
				INFO("MQTT: Queue response QoS: %d\r\n", msg_qos);
				if(QUEUE_Puts(&client->msgQueue, client->mqtt_state.outbound_message->data,\
						client->mqtt_state.outbound_message->length) == -1)
				{
					INFO("MQTT: Queue full\r\n");
				}
			}
			show_package(client->mqtt_state.in_buffer, client->mqtt_state.message_length);
			deliver_publish(client, client->mqtt_state.in_buffer, client->mqtt_state.message_length);
			break;

			case MQTT_MSG_TYPE_PUBACK:
			if(client->mqtt_state.pending_msg_type == MQTT_MSG_TYPE_PUBLISH && client->mqtt_state.pending_msg_id == msg_id)
			{
				INFO("MQTT: received MQTT_MSG_TYPE_PUBACK, finish QoS1 publish\r\n");
			}
			break;

			case MQTT_MSG_TYPE_PUBREC:
			client->mqtt_state.outbound_message = mqtt_msg_pubrel(&client->mqtt_state.mqtt_connection, msg_id);
			if(QUEUE_Puts(&client->msgQueue, client->mqtt_state.outbound_message->data, client->mqtt_state.outbound_message->length) == -1)
			{
				INFO("MQTT: Queue full\r\n");
			}
			break;

			case MQTT_MSG_TYPE_PUBREL:
			client->mqtt_state.outbound_message = mqtt_msg_pubcomp(&client->mqtt_state.mqtt_connection, msg_id);
			if(QUEUE_Puts(&client->msgQueue, client->mqtt_state.outbound_message->data, client->mqtt_state.outbound_message->length) == -1)
			{
				INFO("MQTT: Queue full\r\n");
			}
			break;

			case MQTT_MSG_TYPE_PUBCOMP:
			if(client->mqtt_state.pending_msg_type == MQTT_MSG_TYPE_PUBLISH && client->mqtt_state.pending_msg_id == msg_id)
			{
				INFO("MQTT: receive MQTT_MSG_TYPE_PUBCOMP, finish QoS2 publish\r\n");
			}
			break;

			case MQTT_MSG_TYPE_PINGREQ:
			client->mqtt_state.outbound_message = mqtt_msg_pingresp(&client->mqtt_state.mqtt_connection);
			if(QUEUE_Puts(&client->msgQueue, client->mqtt_state.outbound_message->data, client->mqtt_state.outbound_message->length) == -1)
			{
				INFO("MQTT: Queue full\r\n");
			}
			break;

			case MQTT_MSG_TYPE_PINGRESP:
			INFO("receive a heart beat response!\n");
			client->heart_beat_flag  = 1;
			break;
		}

		break;
	}

	// process package adhesive.
	uint16_t remain_length = client->mqtt_state.message_length_read - client->mqtt_state.message_length;
	client->mqtt_state.message_length_read = remain_length;
	INFO("client->mqtt_state.message_length_read = %d\n", client->mqtt_state.message_length_read);
	INFO("client->mqtt_state.message_length = %d\n", client->mqtt_state.message_length);
	INFO("the package is\n");
	show_package(client->mqtt_state.in_buffer, client->mqtt_state.message_length);
	if(remain_length > 0)
	{
		int i = 0;
		for(i = 0; i< remain_length; i++)
		{
			client->mqtt_state.in_buffer[i] = \
					client->mqtt_state.in_buffer[client->mqtt_state.message_length_read - remain_length + i];
		}

		INFO("Get another published message\r\n");
		goto READPACKET;
	}

	MQTT_Task(client);
}

/**
  * @brief  Client send over callback function.
  * @param  arg: contain the ip link information
  * @retval None
  */
void FUNCTION_ATTRIBUTE
mqtt_tcpclient_sent_cb(void *arg, int8_t error_no)
{
	struct pando_tcp_conn *pCon = (struct pando_tcp_conn *)arg;
	MQTT_Client* client = (MQTT_Client *)pCon->reverse;
	if(error_no == 0)
	{
		INFO("TCP: Sent OK\r\n");
		client->sendTimeout = 0;
		if(client->connState == MQTT_DATA && client->mqtt_state.pending_msg_type == MQTT_MSG_TYPE_PUBLISH){
			if(client->publishedCb)
			client->publishedCb((uint32_t*)client);
		}
	}
	else if(error_no == -1)
	{
		INFO("TCP: sent failed!");
		client->sendTimeout = 0;
		client->connState = TCP_RECONNECT_REQ;
	}
	MQTT_Task(client);
}

void FUNCTION_ATTRIBUTE
mqtt_tcpclient_recon_cb(void *arg, int8_t errType)
{
	struct pando_tcp_conn *pCon = (struct pando_tcp_conn *)arg;
	MQTT_Client* client = (MQTT_Client *)pCon->reverse;

	INFO("TCP: Reconnect to %s:%d\r\n", client->host, client->port);

	client->connState = TCP_RECONNECT_REQ;

	MQTT_Task(client);

}

void FUNCTION_ATTRIBUTE mqtt_timer(void *arg)
{
	MQTT_Client* client = (MQTT_Client*)arg;
    struct data_buf buffer;
    INFO("%s, %d\n", __func__, client->connState);
	if(client->connState == MQTT_DATA){
		client->keepAliveTick ++;
		if(client->keepAliveTick > client->mqtt_state.connect_info->keepalive){
			// check heart beat.
			if(client->heart_beat_flag  == 0)
			{
				client->connState = TCP_RECONNECT_REQ;
			}
			INFO("\r\nMQTT: Send keepalive packet to %s:%d!\r\n", client->host, client->port);
			client->mqtt_state.outbound_message = mqtt_msg_pingreq(&client->mqtt_state.mqtt_connection);
			client->mqtt_state.pending_msg_type = MQTT_MSG_TYPE_PINGREQ;
			client->mqtt_state.pending_msg_type = mqtt_get_type(client->mqtt_state.outbound_message->data);
			client->mqtt_state.pending_msg_id = mqtt_get_id(client->mqtt_state.outbound_message->data, client->mqtt_state.outbound_message->length);


			client->sendTimeout = MQTT_SEND_TIMOUT;
            buffer.length = client->mqtt_state.outbound_message->length;
            buffer.data = client->mqtt_state.outbound_message->data;
			INFO("MQTT: Sending, type: %d, id: %04X\r\n",client->mqtt_state.pending_msg_type, client->mqtt_state.pending_msg_id);
			net_tcp_send(client->pCon, buffer, client->sendTimeout);
			client->mqtt_state.outbound_message = NULL;

			client->keepAliveTick = 0;
			client->heart_beat_flag  = 0;
			MQTT_Task(client);
		}

	} else if(client->connState == TCP_RECONNECT_REQ){
		INFO("%s, client->reconnectTick:%d\n", __func__, client->reconnectTick);
		client->reconnectTick ++;
		if(client->reconnectTick > MQTT_RECONNECT_TIMEOUT) {
			client->reconnectTick = 0;
			client->connState = TCP_RECONNECT;
			MQTT_Task(client);
		}
	}else if(client->connState == TCP_CONNECTING){
		INFO("%s, client->connectTick:%d\n", __func__, client->connectTick);
		client->connectTick ++;
		if(client->connectTick > MQTT_CONNECT_TIMEOUT)
		{
			client->connState = TCP_CONNECTING_ERROR;
			client->connectTick = 0;
			MQTT_Task(client);
		}
	}
	if(client->sendTimeout > 0)
		client->sendTimeout --;
}

void FUNCTION_ATTRIBUTE
mqtt_tcpclient_discon_cb(void *arg, int8_t errno)
{

	struct pando_tcp_conn *pespconn = (struct pando_tcp_conn *)arg;
	MQTT_Client* client = (MQTT_Client *)pespconn->reverse;
	INFO("TCP: Disconnected callback\r\n");
	client->connState = TCP_RECONNECT_REQ;
	if(client->disconnectedCb)
		client->disconnectedCb((uint32_t*)client);

	MQTT_Task(client);
}



/**
  * @brief  Tcp client connect success callback function.
  * @param  arg: contain the ip link information
  * @retval None
  */
void FUNCTION_ATTRIBUTE
mqtt_tcpclient_connect_cb(void *arg, int8_t errno)
{
	struct pando_tcp_conn *pCon = (struct pando_tcp_conn *)arg;
	MQTT_Client* client = (MQTT_Client *)pCon->reverse;
    struct data_buf buffer;
	net_tcp_register_disconnected_callback(pCon, mqtt_tcpclient_discon_cb);
	net_tcp_register_recv_callback(pCon, mqtt_tcpclient_recv);
	net_tcp_register_sent_callback(pCon, mqtt_tcpclient_sent_cb);
	INFO("MQTT: Connected to broker %s:%d\r\n", client->host, client->port);
	mqtt_msg_init(&client->mqtt_state.mqtt_connection, client->mqtt_state.out_buffer, client->mqtt_state.out_buffer_length);
	client->mqtt_state.outbound_message = mqtt_msg_connect(&client->mqtt_state.mqtt_connection, client->mqtt_state.connect_info);
	client->mqtt_state.pending_msg_type = mqtt_get_type(client->mqtt_state.outbound_message->data);
	client->mqtt_state.pending_msg_id = mqtt_get_id(client->mqtt_state.outbound_message->data, client->mqtt_state.outbound_message->length);
	client->sendTimeout = MQTT_SEND_TIMOUT;
    buffer.length = client->mqtt_state.outbound_message->length;
    buffer.data = client->mqtt_state.outbound_message->data;
    INFO("MQTT: Sending, type: %d, id: %04X\r\n",client->mqtt_state.pending_msg_type, client->mqtt_state.pending_msg_id);
    net_tcp_send(pCon, buffer, client->sendTimeout);
	client->mqtt_state.outbound_message = NULL;
	client->connState = MQTT_CONNECT_SENDING;
	MQTT_Task(client);
}

/**
  * @brief  Tcp client connect repeat callback function.
  * @param  arg: contain the ip link information
  * @retval None
  */
/**
  * @brief  MQTT publish function.
  * @param  client: 	MQTT_Client reference
  * @param  topic: 		string topic will publish to
  * @param  data: 		buffer data send point to
  * @param  data_length: length of data
  * @param  qos:		qos
  * @param  retain:		retain
  * @retval TRUE if success queue
  */
BOOL FUNCTION_ATTRIBUTE
MQTT_Publish(MQTT_Client *client, const char* topic, const char* data, int data_length, int qos, int retain)
{
	uint8_t dataBuffer[MQTT_BUF_SIZE];
	uint16_t dataLen;
	client->mqtt_state.outbound_message = mqtt_msg_publish(&client->mqtt_state.mqtt_connection,
										 topic, data, data_length,
										 qos, retain,
										 &client->mqtt_state.pending_msg_id);
	if(client->mqtt_state.outbound_message->length == 0){
		INFO("MQTT: Queuing publish failed\r\n");
		return FALSE;
	}
	INFO("MQTT: queuing publish, length: %d, queue size(%d/%d)\r\n", client->mqtt_state.outbound_message->length, client->msgQueue.rb.fill_cnt, client->msgQueue.rb.size);
	while(QUEUE_Puts(&client->msgQueue, client->mqtt_state.outbound_message->data, client->mqtt_state.outbound_message->length) == -1){
		INFO("MQTT: Queue full\r\n");
		if(QUEUE_Gets(&client->msgQueue, dataBuffer, &dataLen, MQTT_BUF_SIZE) == -1) {
			INFO("MQTT: Serious buffer error\r\n");
			return FALSE;
		}
	}
	MQTT_Task(client);
	return TRUE;
}

/**
  * @brief  MQTT subscibe function.
  * @param  client: 	MQTT_Client reference
  * @param  topic: 		string topic will subscribe
  * @param  qos:		qos
  * @retval TRUE if success queue
  */
BOOL FUNCTION_ATTRIBUTE
MQTT_Subscribe(MQTT_Client *client, char* topic, uint8_t qos)
{
	uint8_t dataBuffer[MQTT_BUF_SIZE];
	uint16_t dataLen;

	client->mqtt_state.outbound_message = mqtt_msg_subscribe(&client->mqtt_state.mqtt_connection,
											topic, 0,
											&client->mqtt_state.pending_msg_id);
	INFO("MQTT: queue subscribe, topic\"%s\", id: %d\r\n",topic, client->mqtt_state.pending_msg_id);
	while(QUEUE_Puts(&client->msgQueue, client->mqtt_state.outbound_message->data, client->mqtt_state.outbound_message->length) == -1){
		INFO("MQTT: Queue full\r\n");
		if(QUEUE_Gets(&client->msgQueue, dataBuffer, &dataLen, MQTT_BUF_SIZE) == -1) {
			INFO("MQTT: Serious buffer error\r\n");
			return FALSE;
		}
	}
	MQTT_Task(client);
	return TRUE;
}


static void FUNCTION_ATTRIBUTE
MQTT_exit(MQTT_Client *client)
{
	if(client == NULL)
	{
		return;
	}
	if(client->host != NULL)
	{
		pd_free(client->host);
		client->host = NULL;
	}
	if(client->connect_info.password != NULL)
	{
		pd_free(client->connect_info.password);
		client->connect_info.password = NULL;
	}
	if(client->connect_info.client_id != NULL)
	{
		pd_free(client->connect_info.client_id);
		client->connect_info.client_id = NULL;
	}
	if(client->connect_info.username != NULL)
	{
		pd_free(client->connect_info.username);
		client->connect_info.username = NULL;
	}
	if(client->mqtt_state.in_buffer != NULL)
	{
		pd_free(client->mqtt_state.in_buffer);
		client->mqtt_state.in_buffer = NULL;
	}
	if(client->mqtt_state.out_buffer != NULL)
	{
		pd_free(client->mqtt_state.out_buffer);
		client->mqtt_state.out_buffer = NULL;
	}
	if(client->msgQueue.buf != NULL)
	{
		pd_free(client->msgQueue.buf);
		client->msgQueue.buf = NULL;
	}
	INFO("mqtt exit:\n");
	if(client->errorCb != NULL)
	{
		(client->errorCb)((uint32_t*)client);
	}
}

void FUNCTION_ATTRIBUTE
MQTT_Task(MQTT_Client *client)
{
	INFO("MQTT TASK: state: %d\n", client->connState);
	uint8_t dataBuffer[MQTT_BUF_SIZE];

	uint16_t dataLen;
    struct data_buf buffer;
	if(client == NULL)
		return;
	switch(client->connState){
	case TCP_RECONNECT_REQ:
		break;
	case TCP_RECONNECT:
		MQTT_Connect(client);
		INFO("TCP: Reconnect to: %s:%d\r\n", client->host, client->port);
		client->connState = TCP_CONNECTING;
		break;
	case MQTT_DATA:
		INFO("MQTT TASK DATA\n");
		if(QUEUE_IsEmpty(&client->msgQueue) || client->sendTimeout != 0) {
			break;
		}
		if(QUEUE_Gets(&client->msgQueue, dataBuffer, &dataLen, MQTT_BUF_SIZE) == 0){
			INFO("%s, dataLen:%d\n", __func__, dataLen);
			client->mqtt_state.pending_msg_type = mqtt_get_type(dataBuffer);
			client->mqtt_state.pending_msg_id = mqtt_get_id(dataBuffer, dataLen);
			client->sendTimeout = MQTT_SEND_TIMOUT;
            buffer.length = dataLen;
            buffer.data = dataBuffer;
			INFO("MQTT: Sending, type: %d, id: %04X\r\n",client->mqtt_state.pending_msg_type, client->mqtt_state.pending_msg_id);
			net_tcp_send(client->pCon, buffer, client->sendTimeout);
			client->mqtt_state.outbound_message = NULL;
			break;
		}
		break;
	case TCP_CONNECTING_ERROR:
		MQTT_Disconnect(client);
		MQTT_exit(client);
		break;
	}
}

/**
  * @brief  MQTT initialization connection function
  * @param  client: 	MQTT_Client reference
  * @param  host: 	Domain or IP string
  * @param  port: 	Port to connect
  * @param  security:		1 for ssl, 0 for none
  * @retval None
  */
void FUNCTION_ATTRIBUTE
MQTT_InitConnection(MQTT_Client *mqttClient, uint8_t* host, uint32_t port, uint8_t security)
{
	uint32_t temp;
	INFO("MQTT_InitConnection\r\n");
	pd_memset(mqttClient, 0, sizeof(MQTT_Client));
	temp = pd_strlen(host);
	mqttClient->host = (uint8_t*)pd_malloc(temp + 1);
    pd_memset(mqttClient->host, 0, temp + 1);
	pd_strcpy(mqttClient->host, host);
	mqttClient->host[temp] = 0;
	mqttClient->port = port;
	mqttClient->security = security;

}

/**
  * @brief  MQTT initialization mqtt client function
  * @param  client: 	MQTT_Client reference
  * @param  clientid: 	MQTT client id
  * @param  client_user:MQTT client user
  * @param  client_pass:MQTT client password
  * @param  client_pass:MQTT keep alive timer, in second
  * @retval None
  */
void FUNCTION_ATTRIBUTE
MQTT_InitClient(MQTT_Client *mqttClient, uint8_t* client_id, uint8_t* client_user, uint8_t* client_pass, uint32_t keepAliveTime, uint8_t cleanSession)
{
	uint32_t temp;
	INFO("MQTT_InitClient\r\n");
	pd_memset(&mqttClient->connect_info, 0, sizeof(mqtt_connect_info_t));

	temp = pd_strlen(client_id);
	mqttClient->connect_info.client_id = (uint8_t*)pd_malloc(temp + 1);
    pd_memset(mqttClient->connect_info.client_id, 0, temp + 1);
	pd_strcpy(mqttClient->connect_info.client_id, client_id);
	mqttClient->connect_info.client_id[temp] = 0;

	temp = pd_strlen(client_user);
	mqttClient->connect_info.username = (uint8_t*)pd_malloc(temp + 1);
	pd_memset(mqttClient->connect_info.username, 0, temp + 1);
	pd_strcpy(mqttClient->connect_info.username, client_user);
	mqttClient->connect_info.username[temp] = 0;

	temp = pd_strlen(client_pass);
	mqttClient->connect_info.password = (uint8_t*)pd_malloc(temp + 1);
	pd_memset(mqttClient->connect_info.password, 0, temp + 1);
	pd_strcpy(mqttClient->connect_info.password, client_pass);
	mqttClient->connect_info.password[temp] = 0;

	mqttClient->connect_info.keepalive = keepAliveTime;
	mqttClient->connect_info.clean_session = cleanSession;

	mqttClient->mqtt_state.in_buffer = (uint8_t *)pd_malloc(MQTT_BUF_SIZE);
    pd_memset(mqttClient->mqtt_state.in_buffer, 0, MQTT_BUF_SIZE);
	mqttClient->mqtt_state.in_buffer_length = MQTT_BUF_SIZE;
	mqttClient->mqtt_state.out_buffer =  (uint8_t *)pd_malloc(MQTT_BUF_SIZE);
    pd_memset(mqttClient->mqtt_state.out_buffer, 0, MQTT_BUF_SIZE);
    mqttClient->mqtt_state.out_buffer_length = MQTT_BUF_SIZE;
	mqttClient->mqtt_state.connect_info = &mqttClient->connect_info;
	mqttClient->mqtt_state.message_length_read = 0;

	mqtt_msg_init(&mqttClient->mqtt_state.mqtt_connection, mqttClient->mqtt_state.out_buffer, mqttClient->mqtt_state.out_buffer_length);
	QUEUE_Init(&mqttClient->msgQueue, QUEUE_BUFFER_SIZE);
    //MQTT_Task(mqttClient);
}

void FUNCTION_ATTRIBUTE
MQTT_InitLWT(MQTT_Client *mqttClient, uint8_t* will_topic, uint8_t* will_msg, uint8_t will_qos, uint8_t will_retain)
{
	uint32_t temp;
	temp = pd_strlen(will_topic);
	mqttClient->connect_info.will_topic = (uint8_t*)pd_malloc(temp + 1);
    pd_memset(mqttClient->connect_info.will_topic, 0, temp + 1);
	pd_strcpy(mqttClient->connect_info.will_topic, will_topic);
	mqttClient->connect_info.will_topic[temp] = 0;

	temp = pd_strlen(will_msg);
	mqttClient->connect_info.will_message = (uint8_t*)pd_malloc(temp + 1);
    pd_memset(mqttClient->connect_info.will_message, 0, temp + 1);
    
	pd_strcpy(mqttClient->connect_info.will_message, will_msg);
	mqttClient->connect_info.will_message[temp] = 0;


	mqttClient->connect_info.will_qos = will_qos;
	mqttClient->connect_info.will_retain = will_retain;
}
/**
  * @brief  Begin connect to MQTT broker
  * @param  client: MQTT_Client reference
  * @retval None
  */
void FUNCTION_ATTRIBUTE
MQTT_Connect(MQTT_Client *mqttClient)
{
	MQTT_Disconnect(mqttClient);
    mqttClient->pCon = (struct pando_tcp_conn *)pd_malloc(sizeof(struct pando_tcp_conn));
    pd_memset(mqttClient->pCon, 0, sizeof(struct pando_tcp_conn));
    (mqttClient->pCon)->reverse = mqttClient;
	mqttClient->pCon->secure = mqttClient->security;
    net_tcp_register_connected_callback(mqttClient->pCon, mqtt_tcpclient_connect_cb);
    //no reconnection call back. TODO
	mqttClient->keepAliveTick = 0;
	mqttClient->reconnectTick = 0;
	mqttClient->connectTick = 0;
	mqttClient->heart_beat_flag = 1;

	mqttClient->mqttTimer.interval = 1000;
	mqttClient->mqttTimer.timer_no = 1;
	mqttClient->mqttTimer.repeated = 1;
	mqttClient->mqttTimer.arg = mqttClient;
	mqttClient->mqttTimer.timer_cb = mqtt_timer;
	pando_timer_init(&(mqttClient->mqttTimer));
	pando_timer_stop(&(mqttClient->mqttTimer));
	pando_timer_start(&(mqttClient->mqttTimer));

	(mqttClient->pCon)->remote_port = mqttClient->port;
	if(UTILS_StrToIP(mqttClient->host, &mqttClient->pCon->remote_ip)) {
		INFO("TCP: Connect to ip %s:%d\r\n", mqttClient->host, mqttClient->port);
		net_tcp_connect(mqttClient->pCon, mqttClient->sendTimeout);

	}
	else {
		INFO("TCP: Connect to domain %s:%d\r\n", mqttClient->host, mqttClient->port);
        //need a host name function. TODO
        //espconn_gethostbyname(mqttClient->pCon, mqttClient->host, &mqttClient->ip, mqtt_dns_found);
	}
	mqttClient->connState = TCP_CONNECTING;
}

void FUNCTION_ATTRIBUTE
MQTT_Disconnect(MQTT_Client *mqttClient)
{
	if(mqttClient->pCon)
	{
		INFO("Free memory\r\n");
		net_tcp_disconnect(mqttClient->pCon);
		pd_free(mqttClient->pCon);
		mqttClient->pCon = NULL;
	}
    pando_timer_stop(&(mqttClient->mqttTimer));
}

void FUNCTION_ATTRIBUTE
MQTT_OnConnected(MQTT_Client *mqttClient, MqttCallback connectedCb)
{
	mqttClient->connectedCb = connectedCb;
}

void FUNCTION_ATTRIBUTE
MQTT_OnConnect_Error(MQTT_Client *mqttClient, MqttCallback error_cb)
{
	mqttClient->errorCb= error_cb;
}

void FUNCTION_ATTRIBUTE
MQTT_OnDisconnected(MQTT_Client *mqttClient, MqttCallback disconnectedCb)
{
	mqttClient->disconnectedCb = disconnectedCb;
}

void FUNCTION_ATTRIBUTE
MQTT_OnData(MQTT_Client *mqttClient, MqttDataCallback dataCb)
{
	mqttClient->dataCb = dataCb;
}

void FUNCTION_ATTRIBUTE
MQTT_OnPublished(MQTT_Client *mqttClient, MqttCallback publishedCb)
{
	mqttClient->publishedCb = publishedCb;
}
