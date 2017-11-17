#include <actions.h>
#include <auth.h>
#include <exceptions.h>

#include <file.h>
#include <logger.h>
#include <version.h>
#include <wait.h>

using suex::permissions::Permissions;
using suex::permissions::User;
using suex::optargs::OptArgs;

#define PATH_EDIT_LOCK PATH_VAR_RUN "/suex/edit.lock"
#define PATH_CONFIG_TMP "/tmp/suex.conf"

void suex::ShowPermissions(const permissions::Permissions &permissions) {
  for (const permissions::Entity &e : permissions) {
    if (e.Owner().Id() != RunningUser().Id() && !permissions.Privileged()) {
      continue;
    }

    std::cout << e << std::endl;
  }
}

const permissions::Entity *suex::Permit(const Permissions &permissions,
                                        const OptArgs &opts) {
  auto perm = permissions.Get(opts.AsUser(), opts.CommandArguments());
  if (perm == nullptr || perm->Deny()) {
    std::stringstream ss;
    throw suex::PermissionError(
        "You are not allowed to execute '%s' as %s",
        utils::CommandArgsText(opts.CommandArguments()).c_str(),
        opts.AsUser().Name().c_str());
  }

  if (perm->PromptForPassword()) {
    std::string cache_token{perm->CacheAuth() ? perm->Command() : ""};
    if (!auth::Authenticate(permissions.AuthStyle(), opts.Interactive(),
                            cache_token)) {
      throw suex::PermissionError("Incorrect password");
    }
  }
  return perm;
}

void suex::SwitchUserAndExecute(const User &user, char *const cmdargv[],
                                char *const envp[]) {
  // update the HOME env according to the as_user dir
  setenv("HOME", user.HomeDirectory().c_str(), 1);

  // set permissions to requested id and gid
  permissions::Set(user);

  // execute with uid and gid. path lookup is done internally, so execvp is not
  // needed.
  execvpe(*cmdargv, &(*cmdargv), envp);
}

void suex::TurnOnVerboseOutput(const permissions::Permissions &permissions) {
  if (!permissions.Privileged()) {
    throw suex::PermissionError(
        "Access denied. You are not allowed to view verbose output.");
  }
  logger::debug().VerboseOn();
  logger::info().VerboseOn();
  logger::warning().VerboseOn();
  logger::error().VerboseOn();
}

void suex::ClearAuthTokens(const Permissions &permissions) {
  int cleared = auth::ClearTokens(permissions.AuthStyle());
  if (cleared < 0) {
    throw std::runtime_error("error while clearing tokens");
  }
  logger::info() << "cleared " << cleared << " tokens" << std::endl;
}

void suex::RemoveEditLock() { file::Remove(PATH_EDIT_LOCK); }

void suex::ShowVersion() { std::cout << "suex: " << VERSION << std::endl; }

void suex::EditConfiguration(const OptArgs &opts,
                             const Permissions &permissions) {
  if (!permissions.Privileged()) {
    throw suex::PermissionError(
        "Access denied. You are not allowed to edit the config file");
  }

  // verbose is needed when editing
  TurnOnVerboseOutput(permissions);

  if (utils::path::Exists(PATH_EDIT_LOCK)) {
    throw suex::PermissionError(
        "suex.conf is being edited from another session");
  }

  if (!auth::Authenticate(permissions.AuthStyle(), true)) {
    throw suex::PermissionError("Incorrect password");
  }

  file::Create(PATH_EDIT_LOCK, true);
  file::Clone(PATH_CONFIG, PATH_CONFIG_TMP, true);
  DEFER({
    file::Remove(PATH_EDIT_LOCK);
    file::Remove(PATH_CONFIG_TMP);
  });

  std::string editor{utils::GetEditor()};
  std::vector<char *> cmdargv{utils::ConstCorrect(editor.c_str()),
                              utils::ConstCorrect(PATH_CONFIG_TMP), nullptr};

  // loop until configuration is valid
  // or user asked to stop
  while (true) {
    pid_t pid = fork();

    if (pid == -1) {
      throw std::runtime_error("fork() error when editing configuration");
    }

    // child process should run the editor
    if (pid == 0) {
      suex::SwitchUserAndExecute(RootUser(), cmdargv.data(), suex::env::Raw());
    }

    // parent process should wait until the child exists
    int status;

    while (-1 == waitpid(pid, &status, 0)) {
      // wait...
    };
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
      throw std::runtime_error("error while waiting for $EDITOR");
    }

    // update the file permissions after editing it
    if (Permissions::Validate(PATH_CONFIG_TMP, opts.AuthStyle())) {
      file::Clone(PATH_CONFIG_TMP, PATH_CONFIG, true);
      std::cout << PATH_CONFIG << " changes applied." << std::endl;
      return;
    }

    std::string prompt{utils::StringFormat(
        "%s is invalid. Do you want to try again?", PATH_CONFIG)};
    if (!utils::AskQuestion(prompt)) {
      std::cout << PATH_CONFIG << " changes discarded." << std::endl;
      return;
    }
  }
}

void suex::CheckConfiguration(const OptArgs &opts) {
  if (opts.CommandArguments().empty()) {
    if (!Permissions::Validate(opts.ConfigPath(), opts.AuthStyle())) {
      throw suex::ConfigError("configuration is not valid");
    }

    if (opts.ConfigPath() == PATH_CONFIG &&
        !file::IsSecure(opts.ConfigPath())) {
      throw suex::ConfigError("configuration file is not secure");
    }

    // done here
    return;
  }

  Permissions perms{opts.ConfigPath(), opts.AuthStyle()};

  auto perm = perms.Get(opts.AsUser(), opts.CommandArguments());
  if (perm == nullptr || perm->Deny()) {
    std::cout << "deny" << std::endl;
    return;
  }
  std::cout << "permit" << (!perm->PromptForPassword() ? " nopass" : "")
            << std::endl;
}
