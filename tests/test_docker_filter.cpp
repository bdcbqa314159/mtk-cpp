#include <doctest/doctest.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "core/toml_filter.hpp"

namespace {

std::string load_docker_filter() {
    std::ifstream in(std::string(MTK_FIXTURES_DIR) + "/../../filters/docker.toml");
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Real `docker pull python:3.12-alpine` output (piped / non-TTY), abridged.
std::string sample_docker_pull() {
    return
        "3.12-alpine: Pulling from library/python\n"
        "bbd8ab56026e: Pulling fs layer\n"
        "5de55e5ef9c0: Pulling fs layer\n"
        "7165d099db60: Pulling fs layer\n"
        "bbd8ab56026e: Download complete\n"
        "7165d099db60: Download complete\n"
        "5de55e5ef9c0: Download complete\n"
        "5de55e5ef9c0: Pull complete\n"
        "7165d099db60: Pull complete\n"
        "bbd8ab56026e: Pull complete\n"
        "Digest: sha256:6d43704baacd1bfbe7c295d7f13079d5d8104ed33568873133f8fc69980419df\n"
        "Status: Downloaded newer image for python:3.12-alpine\n"
        "docker.io/library/python:3.12-alpine\n";
}

// Real `docker build` output (legacy builder), abridged.
std::string sample_docker_build() {
    return
        "Sending build context to Docker daemon  2.048kB\n"
        "Step 1/4 : FROM alpine:3.19\n"
        "5711127a7748: Pulling fs layer\n"
        "5711127a7748: Download complete\n"
        "5711127a7748: Pull complete\n"
        "Status: Downloaded newer image for alpine:3.19\n"
        " ---> 6baf43584bcb\n"
        "Step 2/4 : RUN apk add --no-cache curl\n"
        " ---> Running in 2b273592a053\n"
        "(1/9) Installing ca-certificates (20250911-r0)\n"
        "(9/9) Installing curl (8.14.1-r2)\n"
        "OK: 13 MiB in 24 packages\n"
        " ---> Removed intermediate container 2b273592a053\n"
        " ---> 65152956cba8\n"
        "Step 3/4 : CMD [\"echo\", \"hi\"]\n"
        " ---> Running in 34ae3f3564c5\n"
        " ---> 88c4857cb08b\n"
        "Successfully built 88c4857cb08b\n"
        "Successfully tagged mtk-docker-demo:latest\n";
}

}  // namespace

TEST_CASE("docker filter: pull — drops layer progress, keeps status + ref") {
    auto filters = mtk::core::toml_filter::parse_all(load_docker_filter());
    REQUIRE(filters.size() == 1);
    const auto& f = filters[0];
    CHECK(mtk::core::toml_filter::command_matches(f, "docker"));

    const std::string input = sample_docker_pull();
    const std::string out = mtk::core::toml_filter::apply(f, input);

    CHECK(out.find("Status: Downloaded newer image") != std::string::npos);
    CHECK(out.find("docker.io/library/python:3.12-alpine") != std::string::npos);
    CHECK(out.find("Pulling fs layer") == std::string::npos);
    CHECK(out.find("Pull complete") == std::string::npos);
    CHECK(out.find("Digest: sha256") == std::string::npos);

    const long pct = 100 - static_cast<long>(out.size() * 100 / input.size());
    std::printf("[docker pull spike] bytes_in=%zu bytes_out=%zu savings=%ld%%\n",
                input.size(), out.size(), pct);
    CHECK(out.size() < input.size());
}

TEST_CASE("docker filter: build — drops ceremony, KEEPS steps + RUN output (error signal)") {
    auto filters = mtk::core::toml_filter::parse_all(load_docker_filter());
    REQUIRE(filters.size() == 1);
    const auto& f = filters[0];

    const std::string input = sample_docker_build();
    const std::string out = mtk::core::toml_filter::apply(f, input);

    // Kept: build steps, inner RUN output (where failures live), success tag.
    CHECK(out.find("Step 2/4 : RUN apk add") != std::string::npos);
    CHECK(out.find("Installing curl") != std::string::npos);
    CHECK(out.find("OK: 13 MiB in 24 packages") != std::string::npos);
    CHECK(out.find("Successfully tagged mtk-docker-demo:latest") != std::string::npos);

    // Dropped: build ceremony.
    CHECK(out.find("Sending build context") == std::string::npos);
    CHECK(out.find("--->") == std::string::npos);
    CHECK(out.find("Pulling fs layer") == std::string::npos);

    const long pct = 100 - static_cast<long>(out.size() * 100 / input.size());
    std::printf("[docker build spike] bytes_in=%zu bytes_out=%zu savings=%ld%%\n",
                input.size(), out.size(), pct);
    CHECK(out.size() < input.size());
}
