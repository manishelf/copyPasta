#ifndef LIB_GIT_HPP
#define LIB_GIT_HPP

#include <git2/checkout.h>
#include <git2/clone.h>
#include <git2/reset.h>
#include <git2/diff.h>
#include <git2/patch.h>
#include <git2/signature.h>
#include <git2/types.h>

#include <string>
#include <mutex>
#include <vector>
#include <filesystem>


namespace copypasta {

namespace fs = std::filesystem;

class LibGit {

  using RepoPtr = std::shared_ptr<git_repository>;
  static RepoPtr make_repo(git_repository* repo);

  bool isRepoValid = false;
 
  RepoPtr repo;
  std::string root;
  std::mutex gitMutex;
  
  std::string username = "copyPasta";
  std::string email = "manishelf@proton.me";
  git_signature* signature;

  static std::once_flag lib_git_init;
  static void init();

public:
  LibGit();
  LibGit(git_repository *repo);
  ~LibGit();

  LibGit(LibGit&& other); 
  LibGit(const LibGit& other);

  bool isValid() { return isRepoValid; }

  static LibGit clone(std::string url, std::string path = ".", 
                      bool shallow = false, git_clone_options opts = GIT_CLONE_OPTIONS_INIT);
  static LibGit open(std::string path = ".");
  static LibGit openOrInit(std::string path = ".");
 
  bool isPathIgnored(fs::path path);
  bool isPathIgnored(const std::string& path);
  
  void addIgnoreRule(const std::string& rule);

  void add(const fs::path &path);
  void add(const std::string& path);
  void addAll();

  void checkout(const std::string& blobId, git_checkout_options opts = GIT_CHECKOUT_OPTIONS_INIT);
  
  void setSignature(const std::string& username, const std::string& email);
  void commit(const std::string message);

  void resetHead(git_reset_t opt = GIT_RESET_HARD);

  void branchCreate(const std::string& name); 

  bool branchExists(const std::string& name);

  struct LineDiff {
    git_diff_line_t type = GIT_DIFF_LINE_ADDITION;
    int oldLineNo = 0; // -1 if not in old file i.e add
    int newLineNo = 0; // -1 if not in new file i.e del
    size_t fileOffset = 0; // offset in the original file to the content
    std::string cont;
    std::string blameAuthor;
    std::string blameEmail;
    std::string blameCommit;
  };

  struct Hunk{
    int oldStartLine; 
    int oldLinesCount; 
    int newStartLine; 
    int newLinesCount; 

    // eg @@ -20,1 +20, 3 @@  at start of a hunk 
    std::string header; 

    std::vector<LineDiff> lineDiffs;
  };

  struct FileDiff {
    std::vector<Hunk> hunks;
    std::string oldPath;
    std::string newPath;
    git_delta_t status;
    git_diff_flag_t flags;
  };

  std::vector<FileDiff> diff();
  std::vector<LibGit::FileDiff> diff(std::string fromBlobId, std::string toBlobId,
                                     git_diff_options opts = GIT_DIFF_OPTIONS_INIT); 
};

} // namespace copypasta

#endif // LIB_GIT_HPP
