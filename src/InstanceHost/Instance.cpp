#include "Instance.h"

#include <open_spiel/spiel.h>

#include <nlohmann/json.hpp>

#include <boost/algorithm/string.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unordered_map>

namespace
{
struct GameConfig {
    std::string gameClass;
    std::string board;
    int minPlayers;
    int maxPlayers;
    int moveLimit;
};

template<typename T>
inline void initializeReverseIndex(const std::vector<T> &v, std::unordered_map<T, int> &m)
{
    for (typename std::vector<T>::size_type i = 0; i < v.size(); i++) {
        m[v[i]] = i;
    }
}

std::filesystem::path getConfigPath(const std::string &moduleName)
{
    std::filesystem::path moduleDir(moduleName);
    return moduleDir / std::filesystem::path("config.json");
}

GameConfig parseConfig(const std::filesystem::path &configPath)
{
    std::ifstream ifs(configPath);
    auto config = nlohmann::json::parse(ifs);

    return {
        .gameClass = config["game_class"],
        .board = config["board"],
        .minPlayers = config["min_players"],
        .maxPlayers = config["max_players"],
        .moveLimit = config["move_limit"],
    };
}

std::pair<std::string, std::string> parseGameClass(const std::string &gameClass)
{
    std::vector<std::string> tokens;
    boost::split(tokens, gameClass, boost::is_any_of(":"));
    if (tokens.size() != 2) {
        throw std::runtime_error("invalid game_class format");
    }
    return {tokens[0], tokens[1]};
}

std::string importBoardXml(const std::string &moduleDir, const std::string &boardXmlPath)
{
    std::filesystem::path moduleDirPath(moduleDir);
    auto fullPath = moduleDirPath / std::filesystem::path(boardXmlPath);

    std::stringstream ss;
    std::ifstream uiFile(fullPath);
    ss << uiFile.rdbuf();

    return ss.str();
}

std::shared_ptr<OpenSpielGame> makeOpenSpielGame(
    const std::string &instanceType,
    const GameConfig &config,
    const Board &board,
    const PyGameClass &gameClass,
    int numPlayers)
{
    open_spiel::GameType::Information information;
    if (board.hasPrivateInformation()) {
        information = open_spiel::GameType::Information::kImperfectInformation;
    }
    else {
        information = open_spiel::GameType::Information::kPerfectInformation;
    }

    auto gameType = open_spiel::GameType{
        .short_name = instanceType,
        .long_name = instanceType,
        .dynamics = open_spiel::GameType::Dynamics::kSequential,
        .chance_mode = open_spiel::GameType::ChanceMode::kDeterministic,
        .information = information,
        .utility = open_spiel::GameType::Utility::kZeroSum,
        .reward_model = open_spiel::GameType::RewardModel::kTerminal,
        .max_num_players = config.maxPlayers,
        .min_num_players = config.minPlayers,
        .provides_information_state_string = false,
        .provides_information_state_tensor = false,
        .provides_observation_string = true,
        .provides_observation_tensor = true,
        .parameter_specification = {
            {"num_players", open_spiel::GameParameter(config.minPlayers)}}};

    auto gameInfo = open_spiel::GameInfo{
        .num_distinct_actions = board.numDistinctActions(),
        .max_chance_outcomes = 0,
        .num_players = numPlayers,
        .min_utility = -1.0,
        .max_utility = 1.0 * numPlayers,
        .utility_sum = 0.0,
        .max_game_length = config.moveLimit};

    auto params = open_spiel::GameParameters{
        {"num_players", open_spiel::GameParameter(numPlayers)}};

    return std::make_shared<OpenSpielGame>(gameType, gameInfo, params);
}

} // namespace

Instance::Instance(const std::string &instanceType, const std::vector<std::string> &userIds) :
    userIds(userIds)
{
    initializeReverseIndex(userIds, playerIndices);

    auto configPath = getConfigPath(instanceType);
    auto config = parseConfig(configPath);

    auto boardXml = importBoardXml(instanceType, config.board);
    board = std::make_unique<Board>(boardXml);

    auto [moduleName, className] = parseGameClass(config.gameClass);
    gameClass = std::make_unique<PyGameClass>(moduleName, className);

    openSpielGame = makeOpenSpielGame(instanceType, config, *board, *gameClass, playerIndices.size());
}

void Instance::performAction(const std::string &userId, const std::string &action)
{
    std::unique_lock lock(sm);

    if (!playerIndices.contains(userId)) {
        std::stringstream ss;
        ss << "user " << userId << " is not participating in the game";
        throw std::invalid_argument(ss.str());
    }
    auto playerIndex = playerIndices.at(userId);

    auto gameObject = gameClass->instantiate(*board);
    gameObject->attr(pybind11::str(action))(userId);
}

std::unordered_map<std::string, std::string> Instance::renderMarkup() const
{
    std::shared_lock lock(sm);

    std::unordered_map<std::string, std::string> result;
    for (const auto &userId : userIds) {
        result[userId] = doRenderMarkup(userId);
    }

    return result;
}

std::string Instance::renderMarkup(const std::string &userId) const
{
    std::shared_lock lock(sm);

    if (!playerIndices.contains(userId)) {
        std::stringstream ss;
        ss << "user " << userId << " is not participating in the game";
        throw std::invalid_argument(ss.str());
    }

    return doRenderMarkup(userId);
}

std::string Instance::doRenderMarkup(const std::string &userId) const
{
    auto playerIndex = playerIndices.at(userId);

    return board->render(*gameClass, playerIndex);
}
