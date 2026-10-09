#ifndef ADVMATCH_RESULT_H
#define ADVMATCH_RESULT_H

#include <optional>
#include <string>
#include <utility>

namespace advmatch
{
    template <typename State>
    struct Result
    {
        explicit Result(State state)
            : state(state), errorMessage(std::nullopt)
        {
        }

        explicit Result(State state, std::string errorMessage)
            : state(state), errorMessage(std::move(errorMessage))
        {
        }

        State state;
        std::optional<std::string> errorMessage;
    };

    template <typename State, typename Content>
    struct ResultContent
    {
        explicit ResultContent(State state, Content content)
            : state(state), content(content), errorMessage(std::nullopt)
        {
        }

        explicit ResultContent(State state, Content content, std::string errorMessage)
            : state(state), content(content), errorMessage(std::move(errorMessage))
        {
        }

        explicit ResultContent(State state)
            : state(state), content(std::nullopt), errorMessage(std::nullopt)
        {
        }

        explicit ResultContent(State state, std::string errorMessage)
            : state(state), content(std::nullopt), errorMessage(std::move(errorMessage))
        {
        }

        State state;
        std::optional<Content> content;
        std::optional<std::string> errorMessage;
    };

    template <typename A, typename B>
    struct Pair
    {
        A first;
        B second;
    };
}

#endif // ADVMATCH_RESULT_H
