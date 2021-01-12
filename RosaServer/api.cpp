#include "api.h"
#include <chrono>
#include <filesystem>
#include "console.h"

bool initialized = false;
bool shouldReset = false;

sol::state* lua;
std::string hookMode;

sol::table* playerDataTables[maxNumberOfPlayers];
sol::table* humanDataTables[maxNumberOfHumans];
sol::table* itemDataTables[maxNumberOfItems];
sol::table* vehicleDataTables[maxNumberOfVehicles];
sol::table* bodyDataTables[maxNumberOfRigidBodies];

std::mutex stateResetMutex;

static constexpr const char* errorOutOfRange = "Index out of range";

void printLuaError(sol::error* err) {
	std::ostringstream stream;

	stream << "\033[41;1m Lua error \033[0m\n\033[31m";
	stream << err->what();
	stream << "\033[0m\n";

	Console::log(stream.str());
}

bool noLuaCallError(sol::protected_function_result* res) {
	if (res->valid()) return true;
	sol::error err = *res;
	printLuaError(&err);
	return false;
}

bool noLuaCallError(sol::load_result* res) {
	if (res->valid()) return true;
	sol::error err = *res;
	printLuaError(&err);
	return false;
}

void hookAndReset(int reason) {
	if (Hooks::enabledKeys[Hooks::EnableKeys::ResetGame]) {
		bool noParent = false;
		sol::protected_function func = (*lua)["hook"]["run"];
		if (func != sol::nil) {
			auto res = func("ResetGame", reason);
			if (noLuaCallError(&res)) noParent = (bool)res;
		}
		if (!noParent) {
			{
				subhook::ScopedHookRemove remove(&Hooks::resetGameHook);
				Engine::resetGame();
			}
			if (func != sol::nil) {
				auto res = func("PostResetGame", reason);
				noLuaCallError(&res);
			}
		}
	} else {
		subhook::ScopedHookRemove remove(&Hooks::resetGameHook);
		Engine::resetGame();
	}
}

namespace Lua {
void print(sol::variadic_args args, sol::this_state s) {
	sol::state_view lua(s);

	sol::protected_function toString = lua["tostring"];
	if (toString == sol::nil) {
		return;
	}

	std::ostringstream stream;

	bool doneFirst = false;
	for (auto arg : args) {
		if (doneFirst)
			stream << '\t';
		else
			doneFirst = true;

		auto stringified = toString(arg);

		if (!noLuaCallError(&stringified)) {
			return;
		}

		std::string str = stringified;
		stream << str;
	}

	stream << '\n';

	Console::log(stream.str());
}

void flagStateForReset(const char* mode) {
	hookMode = mode;
	shouldReset = true;
}

Vector Vector_() { return Vector{0.f, 0.f, 0.f}; }

Vector Vector_3f(float x, float y, float z) { return Vector{x, y, z}; }

RotMatrix RotMatrix_(float x1, float y1, float z1, float x2, float y2, float z2,
                     float x3, float y3, float z3) {
	return RotMatrix{x1, y1, z1, x2, y2, z2, x3, y3, z3};
}

static sol::object handleSyncHTTPResponse(httplib::Result& res,
                                          sol::this_state s) {
	sol::state_view lua(s);

	if (res) {
		sol::table table = lua.create_table();
		table["status"] = res->status;
		table["body"] = res->body;

		sol::table headers = lua.create_table();
		for (const auto& h : res->headers) headers[h.first] = h.second;
		table["headers"] = headers;

		return sol::make_object(lua, table);
	}

	return sol::make_object(lua, sol::lua_nil);
}

sol::object http::getSync(const char* scheme, const char* path,
                          sol::table headers, sol::this_state s) {
	httplib::Client client(scheme);
	client.set_connection_timeout(6);
	client.set_keep_alive(false);

	httplib::Headers httpHeaders;
	for (const auto& pair : headers)
		httpHeaders.emplace(pair.first.as<std::string>(),
		                    pair.second.as<std::string>());

	httpHeaders.emplace("Connection", "close");

	auto res = client.Get(path, httpHeaders);
	return handleSyncHTTPResponse(res, s);
}

sol::object http::postSync(const char* scheme, const char* path,
                           sol::table headers, std::string body,
                           const char* contentType, sol::this_state s) {
	httplib::Client client(scheme);
	client.set_connection_timeout(6);
	client.set_keep_alive(false);

	httplib::Headers httpHeaders;
	for (const auto& pair : headers)
		httpHeaders.emplace(pair.first.as<std::string>(),
		                    pair.second.as<std::string>());

	httpHeaders.emplace("Connection", "close");

	auto res = client.Post(path, httpHeaders, body, contentType);
	return handleSyncHTTPResponse(res, s);
}

static inline std::string withoutPostPrefix(std::string name) {
	if (name.rfind("Post", 0) == 0) {
		return name.substr(4);
	}

	return name;
}

bool hook::enable(std::string name) {
	auto search = Hooks::enableNames.find(withoutPostPrefix(name));
	if (search != Hooks::enableNames.end()) {
		Hooks::enabledKeys[search->second] = true;
		return true;
	}
	return false;
}

bool hook::disable(std::string name) {
	auto search = Hooks::enableNames.find(withoutPostPrefix(name));
	if (search != Hooks::enableNames.end()) {
		Hooks::enabledKeys[search->second] = false;
		return true;
	}
	return false;
}

void hook::clear() {
	for (size_t i = 0; i < Hooks::EnableKeys::SIZE; i++) {
		Hooks::enabledKeys[i] = false;
	}
}

void event::sound(int soundType, Vector* pos, float volume, float pitch) {
	Engine::createEventSound(soundType, pos, volume, pitch);
}

void event::soundSimple(int soundType, Vector* pos) {
	Engine::createEventSound(soundType, pos, 1.0f, 1.0f);
}

void event::explosion(Vector* pos) { Engine::createEventExplosion(0, pos); }

void event::bulletHit(int hitType, Vector* pos, Vector* normal) {
	subhook::ScopedHookRemove remove(&Hooks::createEventBulletHitHook);
	Engine::createEventBulletHit(0, hitType, pos, normal);
}

sol::table physics::lineIntersectLevel(Vector* posA, Vector* posB) {
	sol::table table = lua->create_table();
	int res = Engine::lineIntersectLevel(posA, posB);
	if (res) {
		table["pos"] = Engine::lineIntersectResult->pos;
		table["normal"] = Engine::lineIntersectResult->normal;
		table["fraction"] = Engine::lineIntersectResult->fraction;
	}
	table["hit"] = res != 0;
	return table;
}

sol::table physics::lineIntersectHuman(Human* man, Vector* posA, Vector* posB) {
	sol::table table = lua->create_table();
	subhook::ScopedHookRemove remove(&Hooks::lineIntersectHumanHook);
	int res = Engine::lineIntersectHuman(man->getIndex(), posA, posB);
	if (res) {
		table["pos"] = Engine::lineIntersectResult->pos;
		table["normal"] = Engine::lineIntersectResult->normal;
		table["fraction"] = Engine::lineIntersectResult->fraction;
		table["bone"] = Engine::lineIntersectResult->humanBone;
	}
	table["hit"] = res != 0;
	return table;
}

sol::table physics::lineIntersectVehicle(Vehicle* vcl, Vector* posA,
                                         Vector* posB) {
	sol::table table = lua->create_table();
	int res = Engine::lineIntersectVehicle(vcl->getIndex(), posA, posB);
	if (res) {
		table["pos"] = Engine::lineIntersectResult->pos;
		table["normal"] = Engine::lineIntersectResult->normal;
		table["fraction"] = Engine::lineIntersectResult->fraction;

		if (Engine::lineIntersectResult->vehicleFace != -1)
			table["face"] = Engine::lineIntersectResult->vehicleFace;
		else
			table["wheel"] = Engine::lineIntersectResult->humanBone;
	}
	table["hit"] = res != 0;
	return table;
}

sol::object physics::lineIntersectTriangle(Vector* outPos, Vector* normal,
                                           Vector* posA, Vector* posB,
                                           Vector* triA, Vector* triB,
                                           Vector* triC, sol::this_state s) {
	sol::state_view lua(s);

	float outFraction;
	int hit = Engine::lineIntersectTriangle(outPos, normal, &outFraction, posA,
	                                        posB, triA, triB, triC);

	if (hit) return sol::make_object(lua, outFraction);
	return sol::make_object(lua, sol::lua_nil);
}

void physics::garbageCollectBullets() { Engine::bulletTimeToLive(); }

int itemTypes::getCount() { return maxNumberOfItemTypes; }

sol::table itemTypes::getAll() {
	auto arr = lua->create_table();
	for (int i = 0; i < maxNumberOfItemTypes; i++) {
		arr.add(&Engine::itemTypes[i]);
	}
	return arr;
}

ItemType* itemTypes::getByIndex(sol::table self, unsigned int idx) {
	if (idx >= maxNumberOfItemTypes) throw std::invalid_argument(errorOutOfRange);
	return &Engine::itemTypes[idx];
}

int items::getCount() {
	int count = 0;
	for (int i = 0; i < maxNumberOfItems; i++) {
		if ((&Engine::items[i])->active) count++;
	}
	return count;
}

sol::table items::getAll() {
	auto arr = lua->create_table();
	for (int i = 0; i < maxNumberOfItems; i++) {
		auto item = &Engine::items[i];
		if (!item->active) continue;
		arr.add(item);
	}
	return arr;
}

Item* items::getByIndex(sol::table self, unsigned int idx) {
	if (idx >= maxNumberOfItems) throw std::invalid_argument(errorOutOfRange);
	return &Engine::items[idx];
}

Item* items::create(ItemType* type, Vector* pos, RotMatrix* rot) {
	return createVel(type, pos, nullptr, rot);
}

Item* items::createVel(ItemType* type, Vector* pos, Vector* vel,
                       RotMatrix* rot) {
	subhook::ScopedHookRemove remove(&Hooks::createItemHook);
	if (type == nullptr) {
		throw std::invalid_argument("Cannot create item with nil type");
	}

	int id = Engine::createItem(type->getIndex(), pos, vel, rot);

	if (id != -1 && itemDataTables[id]) {
		delete itemDataTables[id];
		itemDataTables[id] = nullptr;
	}

	return id == -1 ? nullptr : &Engine::items[id];
}

Item* items::createRope(Vector* pos, RotMatrix* rot) {
	int id = Engine::createRope(pos, rot);
	return id == -1 ? nullptr : &Engine::items[id];
}

int vehicles::getCount() {
	int count = 0;
	for (int i = 0; i < maxNumberOfVehicles; i++) {
		if ((&Engine::vehicles[i])->active) count++;
	}
	return count;
}

sol::table vehicles::getAll() {
	auto arr = lua->create_table();
	for (int i = 0; i < maxNumberOfVehicles; i++) {
		auto vcl = &Engine::vehicles[i];
		if (!vcl->active) continue;
		arr.add(vcl);
	}
	return arr;
}

Vehicle* vehicles::getByIndex(sol::table self, unsigned int idx) {
	if (idx >= maxNumberOfVehicles) throw std::invalid_argument(errorOutOfRange);
	return &Engine::vehicles[idx];
}

Vehicle* vehicles::create(int type, Vector* pos, RotMatrix* rot, int color) {
	subhook::ScopedHookRemove remove(&Hooks::createVehicleHook);
	int id = Engine::createVehicle(type, pos, nullptr, rot, color);

	if (id != -1 && vehicleDataTables[id]) {
		delete vehicleDataTables[id];
		vehicleDataTables[id] = nullptr;
	}

	return id == -1 ? nullptr : &Engine::vehicles[id];
}

Vehicle* vehicles::createVel(int type, Vector* pos, Vector* vel, RotMatrix* rot,
                             int color) {
	subhook::ScopedHookRemove remove(&Hooks::createVehicleHook);
	int id = Engine::createVehicle(type, pos, vel, rot, color);

	if (id != -1 && vehicleDataTables[id]) {
		delete vehicleDataTables[id];
		vehicleDataTables[id] = nullptr;
	}

	return id == -1 ? nullptr : &Engine::vehicles[id];
}

void chat::announce(const char* message) {
	subhook::ScopedHookRemove remove(&Hooks::createEventMessageHook);
	Engine::createEventMessage(0, (char*)message, -1, 0);
}

void chat::tellAdmins(const char* message) {
	subhook::ScopedHookRemove remove(&Hooks::createEventMessageHook);
	Engine::createEventMessage(4, (char*)message, -1, 0);
}

void chat::addRaw(int type, const char* message, int speakerID, int distance) {
	subhook::ScopedHookRemove remove(&Hooks::createEventMessageHook);
	Engine::createEventMessage(type, (char*)message, speakerID, distance);
}

void accounts::save() {
	subhook::ScopedHookRemove remove(&Hooks::saveAccountsServerHook);
	Engine::saveAccountsServer();
}

int accounts::getCount() {
	int count = 0;
	while (true) {
		Account* acc = &Engine::accounts[count];
		if (!acc->subRosaID) break;
		count++;
	}
	return count;
}

sol::table accounts::getAll() {
	auto arr = lua->create_table();
	for (int i = 0;; i++) {
		Account* acc = &Engine::accounts[i];
		if (!acc->subRosaID) break;
		arr.add(acc);
	}
	return arr;
}

Account* accounts::getByPhone(int phone) {
	for (int i = 0;; i++) {
		Account* acc = &Engine::accounts[i];
		if (!acc->subRosaID) break;
		if (acc->phoneNumber == phone) return acc;
	}
	return nullptr;
}

Account* accounts::getByIndex(sol::table self, unsigned int idx) {
	if (idx >= maxNumberOfAccounts) throw std::invalid_argument(errorOutOfRange);
	return &Engine::accounts[idx];
}

int players::getCount() {
	int count = 0;
	for (int i = 0; i < maxNumberOfPlayers; i++) {
		if ((&Engine::players[i])->active) count++;
	}
	return count;
}

sol::table players::getAll() {
	auto arr = lua->create_table();
	for (int i = 0; i < maxNumberOfPlayers; i++) {
		auto ply = &Engine::players[i];
		if (!ply->active) continue;
		arr.add(ply);
	}
	return arr;
}

Player* players::getByPhone(int phone) {
	for (int i = 0; i < maxNumberOfPlayers; i++) {
		auto ply = &Engine::players[i];
		if (!ply->active) continue;
		if (ply->phoneNumber == phone) return ply;
	}
	return nullptr;
}

sol::table players::getNonBots() {
	auto arr = lua->create_table();
	for (int i = 0; i < maxNumberOfPlayers; i++) {
		auto ply = &Engine::players[i];
		if (!ply->active || !ply->subRosaID || ply->isBot) continue;
		arr.add(ply);
	}
	return arr;
}

Player* players::getByIndex(sol::table self, unsigned int idx) {
	if (idx >= maxNumberOfPlayers) throw std::invalid_argument(errorOutOfRange);
	return &Engine::players[idx];
}

Player* players::createBot() {
	subhook::ScopedHookRemove remove(&Hooks::createPlayerHook);
	int playerID = Engine::createPlayer();
	if (playerID == -1) return nullptr;

	if (playerDataTables[playerID]) {
		delete playerDataTables[playerID];
		playerDataTables[playerID] = nullptr;
	}

	auto ply = &Engine::players[playerID];
	ply->subRosaID = 0;
	ply->isBot = 1;
	ply->team = 6;
	ply->setName("Bot");
	return ply;
}

int humans::getCount() {
	int count = 0;
	for (int i = 0; i < maxNumberOfHumans; i++) {
		if ((&Engine::humans[i])->active) count++;
	}
	return count;
}

sol::table humans::getAll() {
	auto arr = lua->create_table();
	for (int i = 0; i < maxNumberOfHumans; i++) {
		auto man = &Engine::humans[i];
		if (!man->active) continue;
		arr.add(man);
	}
	return arr;
}

Human* humans::getByIndex(sol::table self, unsigned int idx) {
	if (idx >= maxNumberOfHumans) throw std::invalid_argument(errorOutOfRange);
	return &Engine::humans[idx];
}

Human* humans::create(Vector* pos, RotMatrix* rot, Player* ply) {
	int playerID = ply->getIndex();
	if (ply->humanID != -1) {
		subhook::ScopedHookRemove remove(&Hooks::deleteHumanHook);
		Engine::deleteHuman(ply->humanID);
	}
	int humanID;
	{
		subhook::ScopedHookRemove remove(&Hooks::createHumanHook);
		humanID = Engine::createHuman(pos, rot, playerID);
	}
	if (humanID == -1) return nullptr;

	if (humanDataTables[humanID]) {
		delete humanDataTables[humanID];
		humanDataTables[humanID] = nullptr;
	}

	auto man = &Engine::humans[humanID];
	man->playerID = playerID;
	ply->humanID = humanID;
	return man;
}

unsigned int bullets::getCount() { return *Engine::numBullets; }

sol::table bullets::getAll() {
	auto arr = lua->create_table();
	for (unsigned int i = 0; i < *Engine::numBullets; i++) {
		Bullet* bul = &Engine::bullets[i];
		arr.add(bul);
	}
	return arr;
}

int rigidBodies::getCount() {
	int count = 0;
	for (int i = 0; i < maxNumberOfRigidBodies; i++) {
		if ((&Engine::bodies[i])->active) count++;
	}
	return count;
}

sol::table rigidBodies::getAll() {
	auto arr = lua->create_table();
	for (int i = 0; i < maxNumberOfRigidBodies; i++) {
		auto body = &Engine::bodies[i];
		if (!body->active) continue;
		arr.add(body);
	}
	return arr;
}

RigidBody* rigidBodies::getByIndex(sol::table self, unsigned int idx) {
	if (idx >= maxNumberOfRigidBodies)
		throw std::invalid_argument(errorOutOfRange);
	return &Engine::bodies[idx];
}

int bonds::getCount() {
	int count = 0;
	for (int i = 0; i < maxNumberOfBonds; i++) {
		if ((&Engine::bonds[i])->active) count++;
	}
	return count;
}

sol::table bonds::getAll() {
	auto arr = lua->create_table();
	for (int i = 0; i < maxNumberOfBonds; i++) {
		auto bond = &Engine::bonds[i];
		if (!bond->active) continue;
		arr.add(bond);
	}
	return arr;
}

Bond* bonds::getByIndex(sol::table self, unsigned int idx) {
	if (idx >= maxNumberOfBonds) throw std::invalid_argument(errorOutOfRange);
	return &Engine::bonds[idx];
}

int streets::getCount() { return *Engine::numStreets; }

sol::table streets::getAll() {
	auto arr = lua->create_table();
	for (int i = 0; i < *Engine::numStreets; i++) {
		arr.add(&Engine::streets[i]);
	}
	return arr;
}

Street* streets::getByIndex(sol::table self, unsigned int idx) {
	if (idx >= *Engine::numStreets) throw std::invalid_argument(errorOutOfRange);
	return &Engine::streets[idx];
}

int intersections::getCount() { return *Engine::numStreetIntersections; }

sol::table intersections::getAll() {
	auto arr = lua->create_table();
	for (int i = 0; i < *Engine::numStreetIntersections; i++) {
		arr.add(&Engine::streetIntersections[i]);
	}
	return arr;
}

StreetIntersection* intersections::getByIndex(sol::table self,
                                              unsigned int idx) {
	if (idx >= *Engine::numStreetIntersections)
		throw std::invalid_argument(errorOutOfRange);
	return &Engine::streetIntersections[idx];
}

sol::table os::listDirectory(const char* path, sol::this_state s) {
	sol::state_view lua(s);

	auto arr = lua.create_table();
	for (const auto& entry : std::filesystem::directory_iterator(path)) {
		auto table = lua.create_table();
		auto path = entry.path();
		table["isDirectory"] = std::filesystem::is_directory(path);
		table["name"] = path.filename().string();
		table["stem"] = path.stem().string();
		table["extension"] = path.extension().string();
		arr.add(table);
	}
	return arr;
}

bool os::createDirectory(const char* path) {
	return std::filesystem::create_directories(path);
}

double os::realClock() {
	auto now = std::chrono::steady_clock::now();
	auto ms = std::chrono::time_point_cast<std::chrono::milliseconds>(now);
	auto epoch = ms.time_since_epoch();
	auto value = std::chrono::duration_cast<std::chrono::milliseconds>(epoch);
	return value.count() / 1000.;
}

void os::exit() { exitCode(EXIT_SUCCESS); }

void os::exitCode(int code) {
	Console::cleanup();
	::exit(code);
}

uintptr_t memory::baseAddress;

uintptr_t memory::getBaseAddress() { return baseAddress; }

uintptr_t memory::getAddressOfConnection(Connection* address) {
	return (uintptr_t)address;
}

uintptr_t memory::getAddressOfAccount(Account* address) {
	return (uintptr_t)address;
}

uintptr_t memory::getAddressOfPlayer(Player* address) {
	return (uintptr_t)address;
}

uintptr_t memory::getAddressOfHuman(Human* address) {
	return (uintptr_t)address;
}

uintptr_t memory::getAddressOfItemType(ItemType* address) {
	return (uintptr_t)address;
}

uintptr_t memory::getAddressOfItem(Item* address) { return (uintptr_t)address; }

uintptr_t memory::getAddressOfVehicle(Vehicle* address) {
	return (uintptr_t)address;
}

uintptr_t memory::getAddressOfBullet(Bullet* address) {
	return (uintptr_t)address;
}

uintptr_t memory::getAddressOfBone(Bone* address) { return (uintptr_t)address; }

uintptr_t memory::getAddressOfRigidBody(RigidBody* address) {
	return (uintptr_t)address;
}

uintptr_t memory::getAddressOfBond(Bond* address) { return (uintptr_t)address; }

uintptr_t memory::getAddressOfAction(Action* address) {
	return (uintptr_t)address;
}

uintptr_t memory::getAddressOfMenuButton(MenuButton* address) {
	return (uintptr_t)address;
}

uintptr_t memory::getAddressOfStreetLane(StreetLane* address) {
	return (uintptr_t)address;
}

uintptr_t memory::getAddressOfStreet(Street* address) {
	return (uintptr_t)address;
}

uintptr_t memory::getAddressOfStreetIntersection(StreetIntersection* address) {
	return (uintptr_t)address;
}

int8_t memory::readByte(uintptr_t address) { return *(int8_t*)address; }
uint8_t memory::readUByte(uintptr_t address) { return *(uint8_t*)address; }
int16_t memory::readShort(uintptr_t address) { return *(int16_t*)address; }
uint16_t memory::readUShort(uintptr_t address) { return *(uint16_t*)address; }
int32_t memory::readInt(uintptr_t address) { return *(int32_t*)address; }
uint32_t memory::readUInt(uintptr_t address) { return *(uint32_t*)address; }
int64_t memory::readLong(uintptr_t address) { return *(int64_t*)address; }
uint64_t memory::readULong(uintptr_t address) { return *(uint64_t*)address; }
float memory::readFloat(uintptr_t address) { return *(float*)address; }
double memory::readDouble(uintptr_t address) { return *(double*)address; }

std::string memory::readBytes(uintptr_t address, size_t count) {
	return std::string((char*)address, (char*)(address + count));
}

void memory::writeByte(uintptr_t address, int8_t data) {
	*(int8_t*)address = data;
}
void memory::writeUByte(uintptr_t address, uint8_t data) {
	*(uint8_t*)address = data;
}
void memory::writeShort(uintptr_t address, int16_t data) {
	*(int16_t*)address = data;
}
void memory::writeUShort(uintptr_t address, uint16_t data) {
	*(uint16_t*)address = data;
}
void memory::writeInt(uintptr_t address, int32_t data) {
	*(int32_t*)address = data;
}
void memory::writeUInt(uintptr_t address, uint32_t data) {
	*(uint32_t*)address = data;
}
void memory::writeLong(uintptr_t address, int64_t data) {
	*(int64_t*)address = data;
}
void memory::writeULong(uintptr_t address, uint64_t data) {
	*(uint64_t*)address = data;
}
void memory::writeFloat(uintptr_t address, float data) {
	*(float*)address = data;
}
void memory::writeDouble(uintptr_t address, double data) {
	*(double*)address = data;
}

void memory::writeBytes(uintptr_t address, std::string bytes) {
	std::memcpy((void*)address, bytes.data(), bytes.size());
}

};  // namespace Lua

std::string addressFromInteger(unsigned int address) {
	unsigned char* bytes = (unsigned char*)(&address);

	char buf[16];
	sprintf(buf, "%i.%i.%i.%i", (int)bytes[3], (int)bytes[2], (int)bytes[1],
	        (int)bytes[0]);

	return buf;
}

std::string Connection::getAddress() { return addressFromInteger(address); }

std::string Account::__tostring() const {
	char buf[32];
	sprintf(buf, "Account(%i)", getIndex());
	return buf;
}

int Account::getIndex() const {
	return ((uintptr_t)this - (uintptr_t)Engine::accounts) / sizeof(*this);
}

std::string Vector::__tostring() const {
	char buf[64];
	sprintf(buf, "Vector(%f, %f, %f)", x, y, z);
	return buf;
}

Vector Vector::__add(Vector* other) const {
	return {x + other->x, y + other->y, z + other->z};
}

Vector Vector::__sub(Vector* other) const {
	return {x - other->x, y - other->y, z - other->z};
}

Vector Vector::__mul(float scalar) const {
	return {x * scalar, y * scalar, z * scalar};
}

Vector Vector::__mul_RotMatrix(RotMatrix* rot) const {
	return {rot->x1 * x + rot->y1 * y + rot->z1 * z,
	        rot->x2 * x + rot->y2 * y + rot->z2 * z,
	        rot->x3 * x + rot->y3 * y + rot->z3 * z};
}

Vector Vector::__div(float scalar) const {
	return {x / scalar, y / scalar, z / scalar};
}

Vector Vector::__unm() const { return {-x, -y, -z}; }

void Vector::add(Vector* other) {
	x += other->x;
	y += other->y;
	z += other->z;
}

void Vector::mult(float scalar) {
	x *= scalar;
	y *= scalar;
	z *= scalar;
}

void Vector::set(Vector* other) {
	x = other->x;
	y = other->y;
	z = other->z;
}

Vector Vector::clone() const { return Vector{x, y, z}; }

float Vector::dist(Vector* other) const {
	float dx = x - other->x;
	float dy = y - other->y;
	float dz = z - other->z;
	return sqrt(dx * dx + dy * dy + dz * dz);
}

float Vector::distSquare(Vector* other) const {
	float dx = x - other->x;
	float dy = y - other->y;
	float dz = z - other->z;
	return dx * dx + dy * dy + dz * dz;
}

std::string RotMatrix::__tostring() const {
	char buf[256];
	sprintf(buf, "RotMatrix(%f, %f, %f, %f, %f, %f, %f, %f, %f)", x1, y1, z1, x2,
	        y2, z2, x3, y3, z3);
	return buf;
}

RotMatrix RotMatrix::__mul(RotMatrix* other) const {
	return {x1 * other->x1 + y1 * other->x2 + z1 * other->x3,
	        x1 * other->y1 + y1 * other->y2 + z1 * other->y3,
	        x1 * other->z1 + y1 * other->z2 + z1 * other->z3,

	        x2 * other->x1 + y2 * other->x2 + z2 * other->x3,
	        x2 * other->y1 + y2 * other->y2 + z2 * other->y3,
	        x2 * other->z1 + y2 * other->z2 + z2 * other->z3,

	        x3 * other->x1 + y3 * other->x2 + z3 * other->x3,
	        x3 * other->y1 + y3 * other->y2 + z3 * other->y3,
	        x3 * other->z1 + y3 * other->z2 + z3 * other->z3};
}

void RotMatrix::set(RotMatrix* other) {
	x1 = other->x1;
	y1 = other->y1;
	z1 = other->z1;

	x2 = other->x2;
	y2 = other->y2;
	z2 = other->z2;

	x3 = other->x3;
	y3 = other->y3;
	z3 = other->z3;
}

RotMatrix RotMatrix::clone() const {
	return RotMatrix{x1, y1, z1, x2, y2, z2, x3, y3, z3};
}

std::string Player::__tostring() const {
	char buf[16];
	sprintf(buf, "Player(%i)", getIndex());
	return buf;
}

int Player::getIndex() const {
	return ((uintptr_t)this - (uintptr_t)Engine::players) / sizeof(*this);
}

sol::table Player::getDataTable() const {
	int index = getIndex();

	if (!playerDataTables[index]) {
		playerDataTables[index] = new sol::table(lua->lua_state(), sol::create);
	}

	return *playerDataTables[index];
}

void Player::update() const {
	subhook::ScopedHookRemove remove(&Hooks::createEventUpdatePlayerHook);
	Engine::createEventUpdatePlayer(getIndex());
}

void Player::updateFinance() const {
	Engine::createEventUpdatePlayerFinance(getIndex());
}

void Player::remove() const {
	int index = getIndex();

	subhook::ScopedHookRemove remove(&Hooks::deletePlayerHook);
	Engine::deletePlayer(index);

	if (playerDataTables[index]) {
		delete playerDataTables[index];
		playerDataTables[index] = nullptr;
	}
}

void Player::sendMessage(const char* message) const {
	subhook::ScopedHookRemove remove(&Hooks::createEventMessageHook);
	Engine::createEventMessage(6, (char*)message, getIndex(), 0);
}

Human* Player::getHuman() {
	if (humanID == -1) return nullptr;
	return &Engine::humans[humanID];
}

void Player::setHuman(Human* human) {
	if (human == nullptr)
		humanID = -1;
	else
		humanID = human->getIndex();
}

Connection* Player::getConnection() {
	int id = getIndex();
	for (unsigned int i = 0; i < *Engine::numConnections; i++) {
		auto con = &Engine::connections[i];
		if (con->playerID == id) return con;
	}
	return nullptr;
}

Account* Player::getAccount() { return &Engine::accounts[accountID]; }

void Player::setAccount(Account* account) {
	if (account == nullptr)
		throw std::invalid_argument("Cannot set account to nil value");
	else
		accountID = account->getIndex();
}

const Vector* Player::getBotDestination() const {
	if (!botHasDestination) return nullptr;
	return &botDestination;
}

void Player::setBotDestination(Vector* vec) {
	if (vec == nullptr)
		botHasDestination = false;
	else {
		botHasDestination = true;
		botDestination = *vec;
	}
}

Action* Player::getAction(unsigned int idx) {
	if (idx > 63) throw std::invalid_argument(errorOutOfRange);

	return &actions[idx];
}

MenuButton* Player::getMenuButton(unsigned int idx) {
	if (idx > 31) throw std::invalid_argument(errorOutOfRange);

	return &menuButtons[idx];
}

std::string Human::__tostring() const {
	char buf[16];
	sprintf(buf, "Human(%i)", getIndex());
	return buf;
}

int Human::getIndex() const {
	return ((uintptr_t)this - (uintptr_t)Engine::humans) / sizeof(*this);
}

sol::table Human::getDataTable() const {
	int index = getIndex();

	if (!humanDataTables[index]) {
		humanDataTables[index] = new sol::table(lua->lua_state(), sol::create);
	}

	return *humanDataTables[index];
}

void Human::remove() const {
	int index = getIndex();

	subhook::ScopedHookRemove remove(&Hooks::deleteHumanHook);
	Engine::deleteHuman(index);

	if (humanDataTables[index]) {
		delete humanDataTables[index];
		humanDataTables[index] = nullptr;
	}
}

Player* Human::getPlayer() const {
	if (playerID == -1) return nullptr;
	return &Engine::players[playerID];
}

void Human::setPlayer(Player* player) {
	if (player == nullptr)
		playerID = -1;
	else
		playerID = player->getIndex();
}

Vehicle* Human::getVehicle() const {
	if (vehicleID == -1) return nullptr;
	return &Engine::vehicles[vehicleID];
}

void Human::setVehicle(Vehicle* vcl) {
	if (vcl == nullptr)
		vehicleID = -1;
	else
		vehicleID = vcl->getIndex();
}

void Human::teleport(Vector* vec) {
	float offX = vec->x - pos.x;
	float offY = vec->y - pos.y;
	float offZ = vec->z - pos.z;

	Bone* bone;
	RigidBody* body;
	for (int i = 0; i < 16; i++) {
		bone = &bones[i];
		bone->pos.x += offX;
		bone->pos.y += offY;
		bone->pos.z += offZ;
		bone->pos2.x += offX;
		bone->pos2.y += offY;
		bone->pos2.z += offZ;

		body = &Engine::bodies[bone->bodyID];
		body->pos.x += offX;
		body->pos.y += offY;
		body->pos.z += offZ;
	}
};

void Human::speak(const char* message, int distance) const {
	subhook::ScopedHookRemove remove(&Hooks::createEventMessageHook);
	Engine::createEventMessage(1, (char*)message, getIndex(), distance);
}

void Human::arm(int weapon, int magCount) const {
	Engine::scenarioArmHuman(getIndex(), weapon, magCount);
}

Bone* Human::getBone(unsigned int idx) {
	if (idx > 15) throw std::invalid_argument(errorOutOfRange);

	return &bones[idx];
}

RigidBody* Human::getRigidBody(unsigned int idx) const {
	if (idx > 15) throw std::invalid_argument(errorOutOfRange);

	return &Engine::bodies[bones[idx].bodyID];
}

Item* Human::getRightHandItem() const {
	if (!rightHandOccupied) return nullptr;
	return &Engine::items[rightHandItemID];
}

Item* Human::getLeftHandItem() const {
	if (!leftHandOccupied) return nullptr;
	return &Engine::items[leftHandItemID];
}

Human* Human::getRightHandGrab() const {
	if (!isGrabbingRight) return nullptr;
	return &Engine::humans[grabbingRightHumanID];
}

void Human::setRightHandGrab(Human* man) {
	if (man == nullptr)
		isGrabbingRight = 0;
	else {
		isGrabbingRight = 1;
		grabbingRightHumanID = man->getIndex();
		grabbingRightBone = 0;
	}
}

Human* Human::getLeftHandGrab() const {
	if (!isGrabbingLeft) return nullptr;
	return &Engine::humans[grabbingLeftHumanID];
}

void Human::setLeftHandGrab(Human* man) {
	if (man == nullptr)
		isGrabbingLeft = 0;
	else {
		isGrabbingLeft = 1;
		grabbingLeftHumanID = man->getIndex();
		grabbingLeftBone = 0;
	}
}

void Human::setVelocity(Vector* vel) const {
	for (int i = 0; i < 16; i++) {
		auto body = getRigidBody(i);
		body->vel.set(vel);
	}
}

void Human::addVelocity(Vector* vel) const {
	for (int i = 0; i < 16; i++) {
		auto body = getRigidBody(i);
		body->vel.add(vel);
	}
}

bool Human::mountItem(Item* childItem, unsigned int slot) const {
	subhook::ScopedHookRemove remove(&Hooks::linkItemHook);
	return Engine::linkItem(childItem->getIndex(), -1, getIndex(), slot);
}

void Human::applyDamage(int bone, int damage) const {
	subhook::ScopedHookRemove remove(&Hooks::humanApplyDamageHook);
	Engine::humanApplyDamage(getIndex(), bone, 0, damage);
}

std::string ItemType::__tostring() const {
	char buf[16];
	sprintf(buf, "ItemType(%i)", getIndex());
	return buf;
}

int ItemType::getIndex() const {
	return ((uintptr_t)this - (uintptr_t)Engine::itemTypes) / sizeof(*this);
}

std::string Item::__tostring() const {
	char buf[16];
	sprintf(buf, "Item(%i)", getIndex());
	return buf;
}

int Item::getIndex() const {
	return ((uintptr_t)this - (uintptr_t)Engine::items) / sizeof(*this);
}

sol::table Item::getDataTable() const {
	int index = getIndex();

	if (!itemDataTables[index]) {
		itemDataTables[index] = new sol::table(lua->lua_state(), sol::create);
	}

	return *itemDataTables[index];
}

void Item::remove() const {
	int index = getIndex();

	subhook::ScopedHookRemove remove(&Hooks::deleteItemHook);
	Engine::deleteItem(index);

	if (itemDataTables[index]) {
		delete itemDataTables[index];
		itemDataTables[index] = nullptr;
	}
}

Player* Item::getGrenadePrimer() const {
	return grenadePrimerID == -1 ? nullptr : &Engine::players[grenadePrimerID];
}

void Item::setGrenadePrimer(Player* player) {
	grenadePrimerID = player != nullptr ? player->getIndex() : -1;
}

Human* Item::getParentHuman() const {
	return parentHumanID == -1 ? nullptr : &Engine::humans[parentHumanID];
}

Item* Item::getParentItem() const {
	return parentItemID == -1 ? nullptr : &Engine::items[parentItemID];
}

RigidBody* Item::getRigidBody() const { return &Engine::bodies[bodyID]; }

bool Item::mountItem(Item* childItem, unsigned int slot) const {
	subhook::ScopedHookRemove remove(&Hooks::linkItemHook);
	return Engine::linkItem(getIndex(), childItem->getIndex(), -1, slot);
}

bool Item::unmount() const {
	subhook::ScopedHookRemove remove(&Hooks::linkItemHook);
	return Engine::linkItem(getIndex(), -1, -1, 0);
}

void Item::speak(const char* message, int distance) const {
	subhook::ScopedHookRemove remove(&Hooks::createEventMessageHook);
	Engine::createEventMessage(2, (char*)message, getIndex(), distance);
}

void Item::explode() const {
	subhook::ScopedHookRemove remove(&Hooks::grenadeExplosionHook);
	Engine::grenadeExplosion(getIndex());
}

void Item::setMemo(const char* memo) const {
	Engine::itemSetMemo(getIndex(), memo);
}

void Item::computerTransmitLine(unsigned int line) const {
	Engine::itemComputerTransmitLine(getIndex(), line);
}

void Item::computerIncrementLine() const {
	Engine::itemComputerIncrementLine(getIndex());
}

void Item::computerSetLine(unsigned int line, const char* newLine) {
	if (line >= 32) throw std::invalid_argument(errorOutOfRange);
	std::strncpy(computerLines[line], newLine, 63);
}

void Item::computerSetColor(unsigned int line, unsigned int column,
                            unsigned char color) {
	if (line >= 32 || column >= 64) throw std::invalid_argument(errorOutOfRange);
	computerLineColors[line][column] = color;
}

std::string Vehicle::__tostring() const {
	char buf[16];
	sprintf(buf, "Vehicle(%i)", getIndex());
	return buf;
}

int Vehicle::getIndex() const {
	return ((uintptr_t)this - (uintptr_t)Engine::vehicles) / sizeof(*this);
}

sol::table Vehicle::getDataTable() const {
	int index = getIndex();

	if (!vehicleDataTables[index]) {
		vehicleDataTables[index] = new sol::table(lua->lua_state(), sol::create);
	}

	return *vehicleDataTables[index];
}

void Vehicle::updateType() const {
	Engine::createEventCreateVehicle(getIndex());
}

void Vehicle::updateDestruction(int updateType, int partID, Vector* pos,
                                Vector* normal) const {
	subhook::ScopedHookRemove remove(&Hooks::createEventUpdateVehicleHook);
	Engine::createEventUpdateVehicle(getIndex(), updateType, partID, pos, normal);
}

void Vehicle::remove() const {
	int index = getIndex();
	Engine::deleteVehicle(index);

	if (vehicleDataTables[index]) {
		delete vehicleDataTables[index];
		vehicleDataTables[index] = nullptr;
	}
}

Player* Vehicle::getLastDriver() const {
	if (lastDriverPlayerID == -1) return nullptr;
	return &Engine::players[lastDriverPlayerID];
}

RigidBody* Vehicle::getRigidBody() const { return &Engine::bodies[bodyID]; }

Player* Bullet::getPlayer() const {
	if (playerID == -1) return nullptr;
	return &Engine::players[playerID];
}

std::string RigidBody::__tostring() const {
	char buf[16];
	sprintf(buf, "RigidBody(%i)", getIndex());
	return buf;
}

int RigidBody::getIndex() const {
	return ((uintptr_t)this - (uintptr_t)Engine::bodies) / sizeof(*this);
}

sol::table RigidBody::getDataTable() const {
	int index = getIndex();

	if (!bodyDataTables[index]) {
		bodyDataTables[index] = new sol::table(lua->lua_state(), sol::create);
	}

	return *bodyDataTables[index];
}

Bond* RigidBody::bondTo(RigidBody* other, Vector* thisLocalPos,
                        Vector* otherLocalPos) const {
	int id = Engine::createBondRigidBodyToRigidBody(getIndex(), other->getIndex(),
	                                                thisLocalPos, otherLocalPos);
	return id == -1 ? nullptr : &Engine::bonds[id];
}

Bond* RigidBody::bondRotTo(RigidBody* other) const {
	int id =
	    Engine::createBondRigidBodyRotRigidBody(getIndex(), other->getIndex());
	return id == -1 ? nullptr : &Engine::bonds[id];
}

Bond* RigidBody::bondToLevel(Vector* localPos, Vector* globalPos) const {
	int id = Engine::createBondRigidBodyToLevel(getIndex(), localPos, globalPos);
	return id == -1 ? nullptr : &Engine::bonds[id];
}

void RigidBody::collideLevel(Vector* localPos, Vector* normal, float a, float b,
                             float c, float d) const {
	Engine::addCollisionRigidBodyOnLevel(getIndex(), localPos, normal, a, b, c,
	                                     d);
}

std::string Bond::__tostring() const {
	char buf[16];
	sprintf(buf, "Bond(%i)", getIndex());
	return buf;
}

int Bond::getIndex() const {
	return ((uintptr_t)this - (uintptr_t)Engine::bonds) / sizeof(*this);
}

RigidBody* Bond::getBody() const { return &Engine::bodies[bodyID]; }

RigidBody* Bond::getOtherBody() const { return &Engine::bodies[otherBodyID]; }

std::string Street::__tostring() const {
	char buf[16];
	sprintf(buf, "Street(%i)", getIndex());
	return buf;
}

int Street::getIndex() const {
	return ((uintptr_t)this - (uintptr_t)Engine::streets) / sizeof(*this);
}

StreetIntersection* Street::getIntersectionA() const {
	return &Engine::streetIntersections[intersectionA];
}

StreetIntersection* Street::getIntersectionB() const {
	return &Engine::streetIntersections[intersectionB];
}

StreetLane* Street::getLane(unsigned int idx) {
	if (idx >= numLanes) throw std::invalid_argument(errorOutOfRange);

	return &lanes[idx];
}

std::string StreetIntersection::__tostring() const {
	char buf[32];
	sprintf(buf, "StreetIntersection(%i)", getIndex());
	return buf;
}

int StreetIntersection::getIndex() const {
	return ((uintptr_t)this - (uintptr_t)Engine::streetIntersections) /
	       sizeof(*this);
}

Street* StreetIntersection::getStreetEast() const {
	return streetEast == -1 ? nullptr : &Engine::streets[streetEast];
}

Street* StreetIntersection::getStreetSouth() const {
	return streetSouth == -1 ? nullptr : &Engine::streets[streetSouth];
}

Street* StreetIntersection::getStreetWest() const {
	return streetWest == -1 ? nullptr : &Engine::streets[streetWest];
}

Street* StreetIntersection::getStreetNorth() const {
	return streetNorth == -1 ? nullptr : &Engine::streets[streetNorth];
}