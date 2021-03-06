// Byteduino lib - papabyte.com
// MIT License

#include "payment.h"

extern WebSocketsClient webSocketForHub;
extern Byteduino byteduino_device;

bufferPaymentStructure bufferPayment;

cbPaymentResult _cbPaymentResult;
cbBalancesReceived _cbBalancesReceived;

void setCbPaymentResult(cbPaymentResult cbToSet){
	_cbPaymentResult = cbToSet;
}

void setCbBalancesReceived(cbBalancesReceived cbToSet){
	_cbBalancesReceived = cbToSet;
}

int loadBufferPayment(const int amount, const bool hasDataFeed, JsonObject & dataFeed, const char * recipientAddress, const int id){
	if(!bufferPayment.isFree){
#ifdef DEBUG_PRINT
		Serial.println(F("Buffer not free to send payment"));
#endif
		return BUFFER_NOT_FREE;
	} 
	if (MAX_DATA_FEED_JSON_SIZE < dataFeed.measureLength()){
#ifdef DEBUG_PRINT
		Serial.println(F("data feed size exceeds maximum allowed"));
#endif
		return DATA_FEED_TOO_LONG;
	}

	bufferPayment.isFree = false;
	bufferPayment.id = id;
	bufferPayment.amount = amount;
	memcpy(bufferPayment.recipientAddress, recipientAddress,33);
	bufferPayment.arePropsReceived = false;
	bufferPayment.isDefinitionReceived = false;
	bufferPayment.requireDefinition = false;
	bufferPayment.areInputsRequested = false;
	strcpy(bufferPayment.unit,"");
	bufferPayment.isPosted = false;
	dataFeed.printTo(bufferPayment.dataFeedJson);
	bufferPayment.hasDataFeed = hasDataFeed;
	bufferPayment.timeOut = SEND_PAYMENT_TIME_OUT;
	requestDefinition(byteduino_device.fundingAddress);
	getParentsAndLastBallAndWitnesses();
	return SUCCESS;
}

int sendPayment(const int amount, const char * recipientAddress, const int payment_id){
	const int capacity = JSON_OBJECT_SIZE(1);
	StaticJsonBuffer<capacity> jb;
	JsonObject & objDataFeed = jb.createObject(); // dummy object

	if (!isValidChash160(recipientAddress)){
#ifdef DEBUG_PRINT
		Serial.println(F("Recipient address is not valid"));
#endif
		return ADDRESS_NOT_VALID;
	}
	return loadBufferPayment(amount, false, objDataFeed, recipientAddress, payment_id);
}

int postDataFeed(const char * key, const char * value, const int id){
	const int capacity = JSON_OBJECT_SIZE(1);
	StaticJsonBuffer<capacity> jb;
	JsonObject & objDataFeed = jb.createObject();
	objDataFeed[key] = value;
	return loadBufferPayment(0, true, objDataFeed, "", id);
}


void treatPaymentComposition(){
	if (!bufferPayment.isFree && bufferPayment.isDefinitionReceived && bufferPayment.arePropsReceived && !bufferPayment.areInputsRequested){
		requestInputsForAmount(bufferPayment.amount, byteduino_device.fundingAddress);
		bufferPayment.areInputsRequested = true;
	}
}

void managePaymentCompositionTimeOut(){
	if (bufferPayment.timeOut > 0){
		bufferPayment.timeOut--;
	}

	if (bufferPayment.timeOut == 0 && !bufferPayment.isFree){
		if (bufferPayment.isPosted){
			if(_cbPaymentResult){
				_cbPaymentResult(bufferPayment.id, TIMEOUT_PAYMENT_NOT_ACKNOWLEDGED, bufferPayment.unit);
			}
		} else {
			if(_cbPaymentResult){
				_cbPaymentResult(bufferPayment.id, TIMEOUT_PAYMENT_NOT_SENT,"");
			}
		}
		bufferPayment.isFree = true;
#ifdef DEBUG_PRINT
		Serial.println(F("Payment was not sent"));
#endif 
	}
};

void requestDefinition(const char* address){

	char output[140];
	const size_t bufferSize = JSON_ARRAY_SIZE(3) + JSON_OBJECT_SIZE(3) + JSON_OBJECT_SIZE(1)+20;
	StaticJsonBuffer<bufferSize> jsonBuffer;
	JsonArray & mainArray = jsonBuffer.createArray();

	mainArray.add("request");
	JsonObject & objRequest= jsonBuffer.createObject();

	objRequest["command"] = "light/get_definition_for_address";
	JsonObject &objParams = objRequest.createNestedObject("params");
	objParams["address"] = address;
	objRequest["tag"] = getTagWithId(GET_ADDRESS_DEFINITION, bufferPayment.id);

	mainArray.add(objRequest);
	mainArray.printTo(output);
#ifdef DEBUG_PRINT
	Serial.println(output);
#endif
	webSocketForHub.sendTXT(output);
}

int extractIdFromTag(const char * tag){
	String stringTag = tag;
	return stringTag.substring(10).toInt();
}

String getTagWithId(const char * tagType, int id){
	char tag[12];
	getTag(tag, tagType);
	return tag + String(bufferPayment.id);
}

void handlePostJointResponse(JsonObject& receivedObject, const char * tag){
	int id = extractIdFromTag(tag);

	if (receivedObject["response"].is<JsonObject>()){
		if(_cbPaymentResult){
			_cbPaymentResult(id, PAYMENT_REFUSED, bufferPayment.unit);
		}
		bufferPayment.isFree = true;
	} else {
		const char * response = receivedObject["response"];
		if (response != nullptr){
			if (strcmp(response, "accepted") == 0){
				if(_cbPaymentResult){
					_cbPaymentResult(id, PAYMENT_ACKNOWLEDGED, bufferPayment.unit);
				}
				bufferPayment.isFree = true;
			}
		} 
	}
}


void handleDefinition(JsonObject& receivedObject, const char * tag) {
	int id = extractIdFromTag(tag);
	if (receivedObject["response"].is<JsonObject>()){
		const char * definition_chash = receivedObject["response"]["definition_chash"];
		if (definition_chash != nullptr){
			if (strcmp(definition_chash, byteduino_device.fundingAddress) == 0){
				if (receivedObject["response"]["is_stable"].is<bool>()){ 
					if(receivedObject["response"]["is_stable"]){
						if (receivedObject["response"]["definition"].is<JsonArray>()){
							bufferPayment.requireDefinition = false;
						} else {
							bufferPayment.requireDefinition = true;
						}
						bufferPayment.isDefinitionReceived = true;
					} else {
						if (_cbPaymentResult)
							_cbPaymentResult(id, DEFINITION_NOT_STABLE, bufferPayment.unit);
						bufferPayment.isFree = true;
					}
				} else {
	#ifdef DEBUG_PRINT
					Serial.println(F("is_stable flag should be a boolean"));
	#endif
				}
			} else {
				if (_cbPaymentResult)
					_cbPaymentResult(id, CHASH_NOT_MATCHING, bufferPayment.unit);
				bufferPayment.isFree = true;
			}
		} else {
#ifdef DEBUG_PRINT
		Serial.println(F("hub didn't return current definition chash"));
#endif	
		}
	} else {
#ifdef DEBUG_PRINT
		Serial.println(F("response for address definition should be an object"));
#endif
	}
}


void requestInputsForAmount(int amount, const char * address){
#ifdef DEBUG_PRINT
	Serial.println(F("requestInputsForAmount"));
#endif
	const size_t bufferSize = JSON_ARRAY_SIZE(1) + JSON_ARRAY_SIZE(2) + JSON_OBJECT_SIZE(3) + JSON_OBJECT_SIZE(4) + 50;
	StaticJsonBuffer<bufferSize> jsonBuffer;
	JsonArray & mainArray = jsonBuffer.createArray();

	mainArray.add("request");
	JsonObject & objRequest= jsonBuffer.createObject();
	JsonObject & objParams = jsonBuffer.createObject();
	JsonArray & arrAddresses = jsonBuffer.createArray();

	objParams["last_ball_mci"] = bufferPayment.last_ball_mci;
	objParams["amount"] = amount + 1000 + strlen(bufferPayment.dataFeedJson);

	arrAddresses.add(address);
	objParams["addresses"] = arrAddresses;

	objRequest["command"] = "light/pick_divisible_coins_for_amount";
	objRequest["params"] = objParams;
	objRequest["tag"] = getTagWithId(GET_INPUTS_FOR_AMOUNT, bufferPayment.id);

	mainArray.add(objRequest);
	String output;
	mainArray.printTo(output);
#ifdef DEBUG_PRINT
	Serial.println(output);
#endif
	webSocketForHub.sendTXT(output);

}

void handleInputsForAmount(JsonObject& receivedObject, const char * tag) {
	int id = extractIdFromTag(tag);
	if (receivedObject["response"].is<JsonObject>()){
		if (receivedObject["response"]["inputs_with_proofs"].is<JsonArray>()){
			if (receivedObject["response"]["total_amount"].is<int>()){
				if (receivedObject["response"]["total_amount"] > 0){
					if (!bufferPayment.isFree && id == bufferPayment.id){
						composeAndSendUnit(receivedObject["response"]["inputs_with_proofs"], receivedObject["response"]["total_amount"]);
					} else {
#ifdef DEBUG_PRINT
						Serial.println(F("response for inputs doesn't correspond to current payment buffer"));
#endif
					}
				}else{
					if(_cbPaymentResult){
						_cbPaymentResult(id, NOT_ENOUGH_FUNDS, "");
					}
					bufferPayment.isFree = true;
				}

			} else {
#ifdef DEBUG_PRINT
				Serial.println(F("total_amount must be an int"));
#endif
			}
		} else {
#ifdef DEBUG_PRINT
			Serial.println(F("inputs_with_proofs should be an array"));
#endif
		}
	} else {
#ifdef DEBUG_PRINT
		Serial.println(F("response should be an object"));
#endif
	}

}

void composeAndSendUnit(JsonArray& arrInputs, int total_amount){
	
	int headers_commission = 0;
	int payload_commission = 0;
	const bool hasKeySizeUpgrade = byteduino_device.isTestNet || bufferPayment.last_ball_mci >= KEY_SIZE_UPGRADE_MCI;
	
	DynamicJsonBuffer jsonBuffer(1000);
	JsonArray & mainArray = jsonBuffer.createArray();
	JsonObject & objParams = jsonBuffer.createObject();

	JsonObject &unit = objParams.createNestedObject("unit");

	if (byteduino_device.isTestNet){
		unit["version"] = TEST_NET_VERSION;
		unit["alt"] = TEST_NET_ALT;
		headers_commission += 5 + (hasKeySizeUpgrade ? 10 : 0);
						Serial.println(headers_commission);

	} else {
		unit["version"] = MAIN_NET_VERSION;
		unit["alt"] = MAIN_NET_ALT;
		headers_commission += 4 + (hasKeySizeUpgrade ? 10 : 0);
	}

	unit["witness_list_unit"] = (const char *) bufferPayment.witness_list_unit;
	unit["last_ball_unit"] = (const char *) bufferPayment.last_ball_unit;
	unit["last_ball"] = (const char *) bufferPayment.last_ball;
	headers_commission += 132 + (hasKeySizeUpgrade ? 47 : 0);

	unit["timestamp"] = bufferPayment.timestamp;
	headers_commission += 8 + (hasKeySizeUpgrade ? 9 : 0);

	JsonArray & parent_units = jsonBuffer.createArray();
	parent_units.add((const char * ) bufferPayment.parent_units[0]);
	headers_commission+=88 + (hasKeySizeUpgrade ? 12 : 0); // PARENT_UNITS_SIZE
	if (strcmp(bufferPayment.parent_units[1],"") != 0){
		parent_units.add((const char * ) bufferPayment.parent_units[1]);
	}

	headers_commission += 32 + 88 + (hasKeySizeUpgrade ? 21 : 0);// for authors
	if (bufferPayment.requireDefinition)
		headers_commission += 44 + 3 + (hasKeySizeUpgrade ? 16 : 0);// for definition

	JsonObject &datafeedPayload = jsonBuffer.parseObject(bufferPayment.dataFeedJson);
	char datafeedPayloadHash[45];

	if (bufferPayment.hasDataFeed){
		payload_commission += 9 + 6 + 44 + (hasKeySizeUpgrade ? 38 : 0);//data_feed + inline + payload hash
		for (JsonPair& p : datafeedPayload) {
			payload_commission+= strlen(p.value) + (hasKeySizeUpgrade ? strlen(p.key) : 0);
		}

		getBase64HashForJsonObject (datafeedPayloadHash, datafeedPayload, true);
	}

	JsonObject &paymentPayload = jsonBuffer.createObject();
	payload_commission += 7 + 6 + 44 + (hasKeySizeUpgrade ? 46 : 0);//payment + inline + payload hash + messages and payload key

	JsonArray &inputs = paymentPayload.createNestedArray("inputs");

	if (hasKeySizeUpgrade)
		payload_commission+= 6; // for key inputs

	for (JsonObject& elem : arrInputs) {
		JsonObject& input = elem["input"];
		inputs.add(input);
		if (input.containsKey("type"))//if it has a type, then it's header commission
			payload_commission+= 18 + 8 + 8 + (hasKeySizeUpgrade ? 44 : 0); //headers_commission + from_main_chain_index + to_main_chain_index
		else
			payload_commission+= 44 + 8 + 8 + (hasKeySizeUpgrade ? 29 : 0); //unit + message index + output index
	}

	JsonArray &outputs = paymentPayload.createNestedArray("outputs");
	JsonObject &firstOutput = jsonBuffer.createObject();
		if (hasKeySizeUpgrade)
			payload_commission+= 7; // for key outputs
	if (bufferPayment.amount > 0){
		firstOutput["address"] = (const char *) bufferPayment.recipientAddress;
		firstOutput["amount"] = (const int) bufferPayment.amount;
		payload_commission+= 32 + 8 + (hasKeySizeUpgrade ? 13 : 0); // address + amount
	}

	JsonObject &changeOutput = jsonBuffer.createObject();
	changeOutput["address"] = (const char *) byteduino_device.fundingAddress;
	payload_commission+= 32 + 8 + (hasKeySizeUpgrade ? 13 : 0); // address + amount
	int change_amount = total_amount - bufferPayment.amount - headers_commission - payload_commission;
	changeOutput["amount"] = change_amount;

	if (change_amount <= bufferPayment.amount) // we sort outputs by amount in case recipient address is same as change address
		outputs.add(changeOutput);

	if (bufferPayment.amount>0)
		outputs.add(firstOutput);

	if (change_amount > bufferPayment.amount)
		outputs.add(changeOutput);

	char paymentPayloadHash[45];
	getBase64HashForJsonObject (paymentPayloadHash, paymentPayload, true);

	unit["parent_units"] = parent_units;
	JsonArray &messages = jsonBuffer.createArray();

	JsonObject & payment = jsonBuffer.createObject();
	payment["app"] = "payment";
	payment["payload_location"] = "inline";
	payment["payload_hash"] = (const char *) paymentPayloadHash;
	messages.add(payment);

	if (bufferPayment.hasDataFeed){
		JsonObject & datafeed = jsonBuffer.createObject();
		datafeed["app"] = "data_feed";
		datafeed["payload_location"] = "inline";
		datafeed["payload_hash"] = (const char *) datafeedPayloadHash;
		messages.add(datafeed);
	}


	unit["messages"] = messages;
	JsonArray &authors = unit.createNestedArray("authors");
	JsonObject &firstAuthor = jsonBuffer.createObject();
	firstAuthor["address"]=(const char *) byteduino_device.fundingAddress;

	JsonArray &definition = jsonBuffer.createArray();
	if (bufferPayment.requireDefinition){
		JsonObject & objSig = jsonBuffer.createObject();
		objSig["pubkey"] = (const char *) byteduino_device.keys.publicKeyM4400b64;
		definition.add("sig");
		definition.add(objSig);
		firstAuthor["definition"] = definition;
	}

	authors.add(firstAuthor);

	uint8_t hash[32];
	getSHA256ForJsonObject(hash, unit, true);
	char sigb64 [89];
	getB64SignatureForHash(sigb64 ,byteduino_device.keys.privateM4400, hash,32);


	JsonObject &authentifier = jsonBuffer.createObject();
	authentifier["r"] = (const char *) sigb64;
	firstAuthor["authentifiers"] = authentifier;

	char content_hash[45];
	getBase64HashForJsonObject (content_hash, unit, true);

	unit["content_hash"] = (const char *) content_hash;

	firstAuthor.remove("authentifiers");
	firstAuthor.remove("definition");
	unit.remove("messages");
	char unit_hash[45];
	getBase64HashForJsonObject (unit_hash, unit, true);

	unit["unit"] = (const char *) unit_hash;
	strcpy(bufferPayment.unit, unit_hash);
	unit.remove("content_hash");
	unit["messages"] = messages;

	firstAuthor["authentifiers"] = authentifier;
	if (bufferPayment.requireDefinition){
		firstAuthor["definition"] = definition;
	}
	unit["messages"][0]["payload"] = paymentPayload;

	unit["messages"][1]["payload"] = datafeedPayload;
	mainArray.add("request");
	JsonObject & objRequest = jsonBuffer.createObject();

	unit["payload_commission"] = payload_commission;
	unit["headers_commission"] = headers_commission;

	objRequest["command"] = "post_joint";
	objRequest["params"] = objParams;
	objRequest["tag"] =  getTagWithId(POST_JOINT, bufferPayment.id);;
	
	mainArray.add(objRequest);
	String output;
	mainArray.printTo(output);
	Serial.println(output);
	bufferPayment.isPosted = true;
	webSocketForHub.sendTXT(output);

}


void getParentsAndLastBallAndWitnesses(){

	char output[200];
	const size_t bufferSize = JSON_ARRAY_SIZE(2) + JSON_OBJECT_SIZE(4);
	StaticJsonBuffer<bufferSize> jsonBuffer;
	JsonArray & mainArray = jsonBuffer.createArray();
	JsonObject & objParams = jsonBuffer.createObject();

	mainArray.add("request");
	JsonObject & objRequest= jsonBuffer.createObject();

	objRequest["command"] = "light/get_parents_and_last_ball_and_witness_list_unit";
	objRequest["params"] = objParams;

	char tag[12];
	getTag(tag, GET_PARENTS_BALL_WITNESSES);
	objRequest["tag"] = (const char*) tag;
	
	mainArray.add(objRequest);
	mainArray.printTo(output);
#ifdef DEBUG_PRINT
	Serial.println(output);
#endif
	webSocketForHub.sendTXT(output);

}

void handleUnitProps(JsonObject& receivedObject){
	if (receivedObject["response"].is<JsonObject>()){
		JsonObject & response = receivedObject["response"];

		if (response["parent_units"].is<JsonArray>()){
			const char* parent_units_0 = response["parent_units"][0];
			if (parent_units_0 != nullptr) {
				if (strlen(parent_units_0) == 44){
					memcpy(bufferPayment.parent_units[0], parent_units_0,45);
					Serial.println(bufferPayment.parent_units[0]);
				} else {
				
#ifdef DEBUG_PRINT
					Serial.println(F("parent unit should be 44 chars long "));
#endif
					return;
				}
			}
			const char* parent_units_1 = response["parent_units"][1];
			if (parent_units_1 != nullptr) {
				if (strlen(parent_units_1) == 44){
					memcpy(bufferPayment.parent_units[1], parent_units_1,45);
				} else {
#ifdef DEBUG_PRINT
					Serial.println(F("parent unit should be 44 chars long "));
#endif
					return;
				}
			} else{
					strcpy(bufferPayment.parent_units[1], "");
			}
		} else {
#ifdef DEBUG_PRINT
			Serial.println(F("parent_units should be an array"));
#endif
			return;
		}

		const char* last_stable_mc_ball = response["last_stable_mc_ball"];
		if (last_stable_mc_ball != nullptr) {
			if (strlen(last_stable_mc_ball) == 44){
				memcpy(bufferPayment.last_ball, last_stable_mc_ball,45);
			} else {
#ifdef DEBUG_PRINT
				Serial.println(F("last_stable_mc_ball  should be 44 chars long "));
#endif
				return;
			}
		} else {
			return;
#ifdef DEBUG_PRINT
			Serial.println(F("last_stable_mc_ball must be a char"));
#endif
		}

		const char* last_stable_mc_ball_unit = response["last_stable_mc_ball_unit"];
		if (last_stable_mc_ball_unit != nullptr) {
			if (strlen(last_stable_mc_ball_unit) == 44){
				memcpy(bufferPayment.last_ball_unit, last_stable_mc_ball_unit,45);
			} else {
#ifdef DEBUG_PRINT
				Serial.println(F("last_stable_mc_ball_unit  should be 44 chars long "));
#endif
				return;
			}
		} else {
#ifdef DEBUG_PRINT
			Serial.println(F("last_stable_mc_ball_unit must be a char"));
#endif
			return;
		}


		if (response["last_stable_mc_ball_mci"].is<int>()){
			bufferPayment.last_ball_mci = response["last_stable_mc_ball_mci"];
		} else {
#ifdef DEBUG_PRINT
				Serial.println(F("last_stable_mc_ball_mci must be integer "));
#endif
				return;
		}

		if (response["timestamp"].is<int>()){
			bufferPayment.timestamp = response["timestamp"];
		}

		const char* witness_list_unit = response["witness_list_unit"];
		if (witness_list_unit != nullptr) {
			if (strlen(witness_list_unit) == 44){
				memcpy(bufferPayment.witness_list_unit, witness_list_unit,45);
			} else {
				
#ifdef DEBUG_PRINT
				Serial.println(F("witness_list_unit  should be 44 chars long "));
#endif
				return;
			}
		} else {
#ifdef DEBUG_PRINT
			Serial.println(F("witness_list_unit must be a char"));
#endif
			return;
		}
	}else{
#ifdef DEBUG_PRINT
		Serial.println(F("response must be an object"));
#endif
		return;
	}
	bufferPayment.arePropsReceived = true;
}

void getPaymentAddressFromPubKey(const char * pubKey, char * paymentAddress) {
	const size_t bufferSize = JSON_ARRAY_SIZE(2) + JSON_OBJECT_SIZE(4);
	StaticJsonBuffer<bufferSize> jsonBuffer;
	JsonArray & mainArray = jsonBuffer.createArray();
	JsonObject & objSig = jsonBuffer.createObject();
	objSig["pubkey"] = pubKey;
	mainArray.add("sig");
	mainArray.add(objSig);
	getChash160ForArray (mainArray, paymentAddress);
}

void getAvailableBalances(){
	const size_t bufferSize = JSON_ARRAY_SIZE(2) + JSON_OBJECT_SIZE(3) + JSON_ARRAY_SIZE(1);
	StaticJsonBuffer<bufferSize> jsonBuffer;
	JsonArray & mainArray = jsonBuffer.createArray();

	mainArray.add("request");
	JsonObject & objRequest= jsonBuffer.createObject();

	objRequest["command"] = "light/get_balances";
	JsonArray & params = objRequest.createNestedArray("params");
	params.add((const char *) byteduino_device.fundingAddress);
	char tag[12];
	getTag(tag, GET_BALANCE);
	objRequest["tag"] = (const char*) tag;
	
	mainArray.add(objRequest);
	String output;
	mainArray.printTo(output);
#ifdef DEBUG_PRINT
	Serial.println(output);
#endif
	webSocketForHub.sendTXT(output);

}

void handleBalanceResponse(JsonObject& receivedObject){
	if (receivedObject["response"].is<JsonObject>()){
		if (receivedObject["response"][byteduino_device.fundingAddress].is<JsonObject>()){
			if(_cbBalancesReceived){
				_cbBalancesReceived(receivedObject["response"][byteduino_device.fundingAddress]);
			} else {
#ifdef DEBUG_PRINT
				Serial.println(F("no get balance callback set"));
#endif
			}
		} else {
#ifdef DEBUG_PRINT
			Serial.println(F("no balance for my payment address"));
#endif
		}

	} else {
#ifdef DEBUG_PRINT
		Serial.println(F("response must be a object"));
#endif
	}

}
