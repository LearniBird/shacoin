#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <errno.h>

#include <iostream>
#include <list>
#include <string>
#include <functional>
#include <cstdlib>
#include <ctime>

#include <boost/uuid/sha1.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/archive/iterators/base64_from_binary.hpp>
#include <boost/archive/iterators/binary_from_base64.hpp>
#include <boost/archive/iterators/transform_width.hpp>

#include <openssl/rsa.h>
#include <openssl/x509.h>
#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/bio.h>
#include <openssl/buffer.h>

#include "p2pServer.hpp"

/*

block = {
'index': 1,
'timestamp': 1506057125.900785,
'transactions': [
{
'sender': "8527147fe1f5426f9dd545de4b27ee00",
'recipient': "a77f5cdfa2934df3954a5c7c7da5df1f",
'amount': 5,
}
],
'proof': 324984774000,
'previous_hash': "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824"
}

*/

typedef struct __transactions
{
	std::string sender;
	std::string recipient;
	float amount;
}Transactions;

typedef struct __block
{
	int index;
	time_t timestamp;
	std::list<Transactions> lst_ts;
	long int proof;
	std::string previous_hash;
} Block;

typedef struct __keydata
{
	int len;
	unsigned char key[256];
} KeyData;

typedef struct __KeyPair
{
	KeyData prikey;
	KeyData pubKey;
} KeyPair;

static std::list<Block> g_lst_block;
static Node g_selfNode;
static Node g_otherNode;
static char *g_otherIP;
static int g_otherPort;

std::string GetJsonFromBlock(Block &block)
{
	boost::property_tree::ptree item;

	boost::property_tree::ptree lstts;
	{
		std::list<Transactions>::iterator it;
		for (it = block.lst_ts.begin(); it != block.lst_ts.end(); ++it)
		{
			boost::property_tree::ptree ts;
			ts.put("sender", it->sender);
			ts.put("recipient", it->recipient);
			ts.put("amount", it->amount);
			lstts.push_back(make_pair("", ts));
		}
	}

	item.put("index", block.index);
	item.put("timestamp", block.timestamp);
	item.put_child("transactions", lstts);
	item.put("proof", block.proof);
	item.put("previous_hash", block.previous_hash);

	std::stringstream is;
	boost::property_tree::write_json(is, item);
	return is.str();
}

Block GetBlockFromJson(const std::string &json)
{
	Block block;
	std::stringstream ss(json);
	boost::property_tree::ptree pt;
	boost::property_tree::ptree array;
	boost::property_tree::read_json(ss, pt);
	block.index = pt.get<int>("index");
	block.previous_hash = pt.get<std::string>("previous_hash");
	block.proof = pt.get<long int>("proof");
	block.timestamp = pt.get<time_t>("timestamp");
	array = pt.get_child("transactions");

	for (auto v : array)
	{
		Transactions ts;
		ts.sender = v.second.get<std::string>("sender");
		ts.recipient = v.second.get<std::string>("recipient");
		ts.amount = v.second.get<float>("amount");
		block.lst_ts.push_back(ts);
	}

	return block;
}

std::string GetHash(void const* buffer, std::size_t count)
{
	std::stringstream ss;
	boost::uuids::detail::sha1 sha;
	sha.process_bytes(buffer, count);
	unsigned int digest[5];      //ժҪ�ķ���ֵ
	sha.get_digest(digest);
	for (int i = 0; i < 5; ++i)
		ss << std::hex << digest[i];

	return ss.str();
}

std::string Base64Encode(const char*buff, int len)
{
	typedef boost::archive::iterators::base64_from_binary<boost::archive::iterators::transform_width<const char *, 6, 8> > Base64EncodeIterator;
	std::stringstream result;
	std::copy(Base64EncodeIterator(buff), Base64EncodeIterator(buff + len), std::ostream_iterator<char>(result));
	size_t equal_count = (3 - len % 3) % 3;
	for (size_t i = 0; i < equal_count; i++)
	{
		result.put('=');
	}

	return result.str();
}

void Base64Decode(const char *inbuff, int inlen, char *outbuff, int outsize, int *outlen)
{
	if (outsize * 4 / 3 < inlen)
	{
		*outlen = -1;
		return;
	}

	std::stringstream result;

	typedef boost::archive::iterators::transform_width<boost::archive::iterators::binary_from_base64<const char *>, 8, 6> Base64DecodeIterator;
	
	try
	{
		std::copy(Base64DecodeIterator(inbuff), Base64DecodeIterator(inbuff + inlen), std::ostream_iterator<char>(result));
	}
	catch (...)
	{
		return ;
	}
	
	std::string str = result.str();
	*outlen = str.length();
	memcpy((char *)outbuff, str.c_str(), *outlen);
	return ;
}

Transactions CreateTransactions(const std::string &sender, const std::string &recipient, float amount)
{
	Transactions ts;
	ts.sender = sender;
	ts.recipient = recipient;
	ts.amount = amount;
	return ts;
}

Block CreateBlock(int index, time_t timestamp, std::list<Transactions> &lst_ts, long int proof, const std::string &previous)
{
	Block block;
	block.index = index;
	block.timestamp = timestamp;
	block.lst_ts = lst_ts;
	block.proof = proof;
	block.previous_hash = previous;
	return block;
}

void Createkey(KeyPair &keyPair)
{
	unsigned char *p = NULL;
	keyPair.prikey.len = -1;
	keyPair.pubKey.len = -1;
	
	EC_GROUP *group = EC_GROUP_new_by_curve_name(NID_secp256k1);
	if (!group)
		return ;

	EC_KEY *key = EC_KEY_new();
	if (!key)
	{
		EC_GROUP_free(group);
		return;
	}

	if(!EC_KEY_set_group(key, group))
	{
		EC_GROUP_free(group);
		EC_KEY_free(key);
		return;
	}

	if (!EC_KEY_generate_key(key))
	{
		EC_GROUP_free(group);
		EC_KEY_free(key);
		return;
	}

	if (!EC_KEY_check_key(key))
	{
		EC_GROUP_free(group);
		EC_KEY_free(key);
		return;
	}

	keyPair.prikey.len = i2d_ECPrivateKey(key, NULL);
	if (keyPair.prikey.len > (int)sizeof(keyPair.prikey.key))
	{
		keyPair.prikey.len = -1;
		EC_GROUP_free(group);
		EC_KEY_free(key);
		return;
	}
	p = keyPair.prikey.key;
	keyPair.prikey.len = i2d_ECPrivateKey(key, &p);

	keyPair.pubKey.len = i2o_ECPublicKey(key, NULL);
	if (keyPair.pubKey.len > (int)sizeof(keyPair.pubKey.key))
	{
		keyPair.pubKey.len = -1;
		EC_GROUP_free(group);
		EC_KEY_free(key);
		return;
	}
	p = keyPair.pubKey.key;
	keyPair.pubKey.len = i2o_ECPublicKey(key, &p);

	EC_GROUP_free(group);
	EC_KEY_free(key);
}

std::string CreateNewAddress(const KeyPair &keyPair)
{
	std::string hash = GetHash(keyPair.pubKey.key, keyPair.pubKey.len);
	return Base64Encode(hash.c_str(), hash.length());
}

int Signature(const KeyData &prikey, const char *data, int datalen, unsigned char *sign, size_t signszie, unsigned int *signlen)
{
	EC_KEY *ec_key = NULL;
	const unsigned char *pp = (const unsigned char *)prikey.key;
	ec_key = d2i_ECPrivateKey(&ec_key, &pp, prikey.len);
	if (!ec_key)
		return 0;

	if (ECDSA_size(ec_key) > (int)signszie)
	{
		EC_KEY_free(ec_key);
		return 0;
	}

	if (!ECDSA_sign(0, (unsigned char *)data, datalen, sign, signlen, ec_key))
	{
		EC_KEY_free(ec_key);
		return 0;
	}

	EC_KEY_free(ec_key);
	return 1;
}

int Verify(const KeyData &pubkey, const char *data, int datalen, const unsigned char *sign, size_t signszie, unsigned int signlen)
{
	int ret = -1;
	EC_KEY *ec_key = NULL;
	EC_GROUP *ec_group = NULL;
	const unsigned char *pp = (const unsigned char *)pubkey.key;

	ec_key = EC_KEY_new();
	if (!ec_key)
		return 0;

	if (ECDSA_size(ec_key) > (int)signszie)
	{
		EC_KEY_free(ec_key);
		return 0;
	}

	ec_group = EC_GROUP_new_by_curve_name(NID_secp256k1);
	if (!ec_group)
	{
		EC_KEY_free(ec_key);
		return 0;
	}

	if (!EC_KEY_set_group(ec_key, ec_group))
	{
		EC_GROUP_free(ec_group);
		EC_KEY_free(ec_key);
		return 0;
	}

	ec_key = o2i_ECPublicKey(&ec_key, &pp, pubkey.len);
	if (!ec_key)
	{
		EC_GROUP_free(ec_group);
		EC_KEY_free(ec_key);
		return 0;
	}

	ret = ECDSA_verify(0, (const unsigned char*)data, datalen, sign,
		signlen, ec_key);

	EC_GROUP_free(ec_group);
	EC_KEY_free(ec_key);
	return ret;
}

int main(int argc, char **argv)
{
// 	if (lst_block.size() == 0)
// 	{
// 		Block GenesisBlock;
// 		GenesisBlock.index = 0;
// 		GenesisBlock.timestamp = time(NULL);
// 		GenesisBlock.lst_ts.clear();
// 		GenesisBlock.proof = 100;
// 		GenesisBlock.previous_hash = "1";
// 		lst_block.push_back(GenesisBlock);
// 	}
// 
// 	KeyPair keyPair;
//  	Createkey(keyPair);
//  	std::string addr = CreateNewAddress(keyPair);
// 
// 	std::string addrhash = GetHash(addr.c_str(), addr.length());
// 	unsigned int signlen = 0;
// 	unsigned char sign[1024] = { 0 };
// 	int a = Signature(keyPair.prikey, addrhash.c_str(), addrhash.length(), sign, sizeof(sign), &signlen);
// 	int b = Verify(keyPair.pubKey, addrhash.c_str(), addrhash.length(), sign, sizeof(sign), signlen);

	int sock = socket(AF_INET, SOCK_DGRAM, 0);//IPV4  SOCK_DGRAM ���ݱ��׽��֣�UDPЭ�飩  
	if (sock < 0)
	{
		perror("socket\n");
		return 2;
	}

	int val = 1;
	if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val)) < 0)
	{
		perror("setsockopt");
	}

	struct timeval tv_out;
	tv_out.tv_sec = 10;//�ȴ�10��
	tv_out.tv_usec = 0;
	if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv_out, sizeof(tv_out)) < 0)
	{
		perror("setsockopt");
	}

	struct sockaddr_in serverAddr;
	serverAddr.sin_family = AF_INET;
	serverAddr.sin_port = htons(SERVERPORT);
	serverAddr.sin_addr.s_addr = inet_addr(SERVERIP);
	socklen_t serverLen = sizeof(serverAddr);

	struct sockaddr_in localAddr;
	localAddr.sin_family = AF_INET;
	localAddr.sin_port = htons(LOCALPORT);
	localAddr.sin_addr.s_addr = htonl(INADDR_ANY);
	if (bind(sock, (struct sockaddr*)&localAddr, sizeof(localAddr)) < 0)
	{
		perror("bind");
		close(sock);
		exit(2);
	}

	sockaddr_in recvAddr;
	memset(&recvAddr, 0, sizeof(recvAddr));
	socklen_t recvLen = sizeof(recvAddr);

	NodeInfo sendNode, recvNode;
	memset(&sendNode, 0, sizeof(NodeInfo));
	memset(&recvNode, 0, sizeof(NodeInfo));
	sendNode.cmd = cmd_register;
	sendNode.node.recvPort = LOCALPORT;
	get_local_ip(argv[1], sendNode.node.recvIp);

	while (1)
	{
		if (sendto(sock, &sendNode, sizeof(NodeInfo), 0, (struct sockaddr*)&serverAddr, serverLen) < 0)
		{
			perror("sendto");
			close(sock);
			exit(4);
		}

		memset(&recvAddr, 0, sizeof(recvAddr));
		memset(&recvNode, 0, sizeof(NodeInfo));
		if (recvfrom(sock, &recvNode, sizeof(NodeInfo), 0, (struct sockaddr*)&recvAddr, &recvLen) < 0)
		{
			if (errno == EINTR || errno == EAGAIN)
				continue;

			perror("recvfrom");
			close(sock);
			exit(2);
		}

		if (recvNode.cmd != cmd_register)
			continue;

		g_selfNode = recvNode.node;
		break;
	}

	while (1)
	{
		memset(&sendNode, 0, sizeof(NodeInfo));
		memset(&recvNode, 0, sizeof(NodeInfo));
		memset(&recvAddr, 0, sizeof(recvAddr));

		sendNode.cmd = cmd_getnode;
		sendNode.node = g_selfNode;
		if (sendto(sock, &sendNode, sizeof(NodeInfo), 0, (struct sockaddr*)&serverAddr, serverLen) < 0)
		{
			perror("sendto");
			close(sock);
			exit(4);
		}

		if (recvfrom(sock, &recvNode, sizeof(NodeInfo), 0, (struct sockaddr*)&recvAddr, &recvLen) < 0)
		{
			if (errno == EINTR || errno == EAGAIN)
				continue;

			perror("recvfrom");
			close(sock);
			exit(2);
		}

		if (recvNode.cmd != cmd_getnode)
			continue;

		g_otherNode = recvNode.node;
		break;
	}

	//��ѯ����IP�ͽ��յ���IP�������ͬ��������������
	//�Լ���ѯ����IP����һ���ڵ��ѯ����IP�����ͬ������ͬһ����
	//ͬһ�����Ľڵ��������������ݣ�����ͬһ�����Ľڵ�ֻ���ù�����������
	if (strcmp(g_selfNode.queryIp, g_selfNode.recvIp) && 
		!strcmp(g_selfNode.queryIp, g_otherNode.queryIp))
	{
		g_otherIP = g_otherNode.recvIp;
		g_otherPort = g_otherNode.recvPort;
	}
	else
	{
		g_otherIP = g_otherNode.queryIp;
		g_otherPort = g_otherNode.queryPort;
	}

	sockaddr_in otherAddr;
	memset(&otherAddr, 0, sizeof(otherAddr));
	otherAddr.sin_family = AF_INET;
	otherAddr.sin_port = htons(g_otherPort);
	otherAddr.sin_addr.s_addr = inet_addr(g_otherIP);

	char recvbuff[128] = { 0 };
	char sendbuff[128] = { 0 };
	sprintf(sendbuff, "This is a message sent from %s:%d to %s:%d.",
		g_selfNode.recvIp, g_selfNode.recvPort, g_otherNode.queryIp, g_otherNode.queryPort);

	while (1)
	{
		memset(&recvAddr, 0, sizeof(recvAddr));

		if (sendto(sock, sendbuff, strlen(sendbuff) + 1, 0, (struct sockaddr*)&otherAddr, sizeof(otherAddr)) < 0)
		{
			perror("sendto");
			close(sock);
			exit(4);
		}

		if (recvfrom(sock, recvbuff, sizeof(recvbuff), 0, (struct sockaddr*)&recvAddr, &recvLen) < 0)
		{
			if (errno == EINTR || errno == EAGAIN)
				continue;

			perror("recvfrom");
			close(sock);
			exit(2);
		}

		std::cout << recvbuff << std::endl;
	}

	memset(&sendNode, 0, sizeof(NodeInfo));
	sendNode.cmd = cmd_unregister;
	sendNode.node = g_selfNode;
	if (sendto(sock, &sendNode, sizeof(NodeInfo), 0, (struct sockaddr*)&serverAddr, serverLen) < 0)
	{
		perror("sendto"); 
		close(sock);
		exit(4);
	}

	close(sock);
	return 0;
}
