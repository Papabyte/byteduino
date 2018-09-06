// Byteduino lib - papabyte.com
// MIT License

#include "wallet.h"

extern walletCreation newWallet;
extern Byteduino byteduino_device;
extern bufferPackageSent bufferForPackageSent;

void readWalletsJson(char * json){
	const char firstChar = EEPROM.read(WALLETS_CREATED);
	if (firstChar == 0x7B){
		int i = -1; 
		do {
			i++;
			json[i] = EEPROM.read(WALLETS_CREATED+i);
		}
		while (json[i] != 0x00 && i < (WALLETS_CREATED_FLASH_SIZE));
		json[WALLETS_CREATED_FLASH_SIZE]=0x00;
	}else{
		json[0] = 0x7B;
		json[1] = 0x7D;
		json[2] = 0x00;
	}
	
}

void saveWalletDefinitionInFlash(const char* wallet,const char* wallet_name, JsonArray& wallet_definition_template){
	char output[WALLETS_CREATED_FLASH_SIZE];
	char input[WALLETS_CREATED_FLASH_SIZE];
	char templateString[WALLETS_CREATED_FLASH_SIZE/2];
	DynamicJsonBuffer jb(500);
	wallet_definition_template.printTo(templateString);
	readWalletsJson(input);
	Serial.println(input);
	JsonObject& objectWallets = jb.parseObject(input);
	
	if (objectWallets.success()){
		if (objectWallets.containsKey(wallet)){
			if (objectWallets["wallet"].is<JsonObject>()){
				objectWallets["wallet"]["name"] = wallet_name;
				objectWallets["wallet"]["definition"] = (const char*) templateString;
			}
		} else{
			DynamicJsonBuffer jb2(250);
			JsonObject& objectWallet = jb2.createObject();
			objectWallet["name"] = wallet_name;
			objectWallet["definition"] = (const char*) templateString;
			objectWallets[wallet] = objectWallet;
		}

		ESP.wdtFeed();
		if (objectWallets.measureLength() < WALLETS_CREATED_FLASH_SIZE){
			objectWallets.printTo(output);
		} else {
			return;
	#ifdef DEBUG_PRINT
		Serial.println(F("No flash available to store wallet"));
	#endif
		}
	#ifdef DEBUG_PRINT
		Serial.println(F("Save wallet in flash"));
		Serial.println(output);
	#endif

		int i = -1; 
		do {
			i++;
			EEPROM.write(WALLETS_CREATED+i, output[i]);
		}
		while (output[i]!= 0x00 && i < (WALLETS_CREATED+WALLETS_CREATED_FLASH_SIZE));
		ESP.wdtFeed();
		EEPROM.commit();
		ESP.wdtFeed();

	}else{
#ifdef DEBUG_PRINT
	Serial.println(F("Impossible to parse objectWallets"));
#endif
	}
}


void handleNewWalletRequest(char initiatiorPubKey [45], JsonObject& package){

	const char* wallet = package["body"]["wallet"];
	if (wallet != nullptr){
		
		if(package["body"]["other_cosigners"].is<JsonArray>()){
			if (package["body"]["other_cosigners"].size() > 0){
				newWallet.isCreating = true;
				memcpy(newWallet.initiatorPubKey,initiatiorPubKey,45);
				memcpy(newWallet.id,wallet,45);
			
				int otherCosignersSize = package["body"]["other_cosigners"].size();
				for (int i; i < otherCosignersSize;i++){
					const  char* device_address = package["body"]["other_cosigners"][i]["device_address"];
					const  char* pubkey = package["body"]["other_cosigners"][i]["pubkey"];
					
					if (pubkey != nullptr && device_address != nullptr){

						if (strcmp(device_address,byteduino_device.deviceAddress) != 0){

							newWallet.xPubKeyQueue[i].isFree = false;
							memcpy(newWallet.xPubKeyQueue[i].recipientPubKey,pubkey,45);
						}

					}
				}
				newWallet.xPubKeyQueue[otherCosignersSize+1].isFree = false;
				memcpy(newWallet.xPubKeyQueue[otherCosignersSize+1].recipientPubKey,initiatiorPubKey,45);

				const char* wallet_name = package["body"]["wallet_name"];
				if (wallet_name != nullptr && package["body"]["wallet_definition_template"].is<JsonArray>()){
					saveWalletDefinitionInFlash(wallet, wallet_name, package["body"]["wallet_definition_template"]);
				} else {
#ifdef DEBUG_PRINT
			Serial.println(F("wallet_definition_template and wallet_name must be char"));
#endif
			}

			} else{
#ifdef DEBUG_PRINT
			Serial.println(F("other_cosigners cannot be empty"));
#endif
			}
		}else{
#ifdef DEBUG_PRINT
			Serial.println(F("other_cosigners must be an array"));
#endif
		}
	}else{
#ifdef DEBUG_PRINT
			Serial.println(F("Wallet must be a char"));
#endif
	}

}


void treatNewWalletCreation(){

	if (newWallet.isCreating){
		bool isQueueEmpty = true;
		
		//we send our extended pubkey to every wallet cosigners
		for (int i=0;i<MAX_COSIGNERS;i++){
			
			if (!newWallet.xPubKeyQueue[i].isFree){
				isQueueEmpty = false;
				if(sendXpubkeyTodevice(newWallet.xPubKeyQueue[i].recipientPubKey)){
					newWallet.xPubKeyQueue[i].isFree = true;
				}
			}
		}
		if (isQueueEmpty){
			if (sendWalletFullyApproved(newWallet.initiatorPubKey)){
				newWallet.isCreating = false;
			}
		}
	}
}
	
bool sendWalletFullyApproved(const char recipientPubKey[45]){
	
	if (bufferForPackageSent.isFree){
		char output[200];
		StaticJsonBuffer<400> jsonBuffer;
		JsonObject & message = jsonBuffer.createObject();
		message["from"] = byteduino_device.deviceAddress;
		message["device_hub"] = byteduino_device.hub;
		message["subject"] = "wallet_fully_approved";

		JsonObject & objBody = jsonBuffer.createObject();
		objBody["wallet"]= newWallet.id;
		message["body"]= objBody;

		bufferForPackageSent.isRecipientTempMessengerKeyKnown = false;
		memcpy(bufferForPackageSent.recipientPubkey,recipientPubKey,45);
		memcpy(bufferForPackageSent.recipientHub,byteduino_device.hub,strlen(byteduino_device.hub)+1);
		bufferForPackageSent.isFree = false;
		bufferForPackageSent.isRecipientKeyRequested = false;
		message.printTo(bufferForPackageSent.message);
		Serial.println(bufferForPackageSent.message);
		return true;
	} else {
#ifdef DEBUG_PRINT
			Serial.println(F("Buffer not free to send message"));
#endif
		return false;
	}
	
	
}

bool sendXpubkeyTodevice(const char recipientPubKey[45]){

	if (bufferForPackageSent.isFree){
		char output[200];
		StaticJsonBuffer<400> jsonBuffer;
		JsonObject & message = jsonBuffer.createObject();
		message["from"] = byteduino_device.deviceAddress;
		message["device_hub"] = byteduino_device.hub;
		message["subject"] = "my_xpubkey";
		
		JsonObject & objBody = jsonBuffer.createObject();
		objBody["wallet"]= newWallet.id;
		objBody["my_xpubkey"]= byteduino_device.keys.extPubKey;
		message["body"]= objBody;
		
		bufferForPackageSent.isRecipientTempMessengerKeyKnown = false;
		memcpy(bufferForPackageSent.recipientPubkey,recipientPubKey,45);
		memcpy(bufferForPackageSent.recipientHub,byteduino_device.hub,strlen(byteduino_device.hub)+1);
		bufferForPackageSent.isFree = false;
		bufferForPackageSent.isRecipientKeyRequested = false;
		message.printTo(bufferForPackageSent.message);
		Serial.println(bufferForPackageSent.message);
		return true;
	} else {
#ifdef DEBUG_PRINT
		Serial.println(F("Buffer not free to send pubkey"));
#endif
		return false;
	}

	
}
