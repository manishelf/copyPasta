#include <LibGit.hpp>
#include <Logger.hpp>
#include <assert.h>
#include <git2/index.h>
#include <git2/ignore.h>
#include <git2/errors.h>
#include <git2/branch.h>
#include <git2/blame.h>
#include <git2/revparse.h>
#include <git2/global.h>

namespace copypasta {

    std::once_flag LibGit::lib_git_init; // needs a global instance to track init

    void LibGit::init() {
        std::call_once(lib_git_init, +[]() {
            DEBUG("LibGit init");
            git_libgit2_init();
            });
    }

    LibGit::LibGit() {
        isRepoValid = false;
    }

    LibGit::LibGit(git_repository* repo) {
        assert(repo != nullptr);
        DEBUG_FULL("LibGit ctor");
        init();
        isRepoValid = true;
        this->repo = make_repo(repo);
        root = git_repository_workdir(repo);
        setSignature(username, email);
    }

    LibGit::LibGit(LibGit&& other) : repo(std::move(other.repo))
        , root(std::move(other.root))
        , username(std::move(other.username))
        , email(std::move(other.email))
        , signature(other.signature)
        // mutex is default-constructed fresh — cannot be moved
    {
        DEBUG_FULL("LibGit move ctor");
        isRepoValid = true;
        other.isRepoValid = false;
        other.signature = nullptr;
    }

    LibGit::LibGit(const LibGit& other) {
        DEBUG_FULL("LibGit copy ctor");
        root = other.root;
        username = other.username;
        email = other.email;
        setSignature(username, email);
        LibGit temp = LibGit::open(other.root);
        repo = std::move(temp.repo);
        isRepoValid = other.isRepoValid;
    }

    LibGit::~LibGit() {
        DEBUG_FULL("LibGit destroyed");
        if (signature) {
            git_signature_free(signature);
        }
    }

    LibGit::RepoPtr LibGit::make_repo(git_repository* raw) {
        return RepoPtr(raw, [](git_repository* r) {
            git_repository_free(r);
            // this will free when the last instance of LibGit using this is delete
            // also this is thread safe
            });
    }

    LibGit LibGit::clone(std::string url, std::string path, bool shallow, git_clone_options opts) {
        init();
        DEBUG("LibGit clone start " << url << " to path " << path);
        git_repository* repo = nullptr;

        if (shallow) {
            opts.fetch_opts.depth = 1;
        }

        if (git_clone(&repo, url.c_str(), path.c_str(), &opts) < 0) {
            auto e = git_error_last();
            auto repo = LibGit();
            return repo;
            /*throw std::runtime_error(std::string("Unable to clone repository at " + url + " due to :") +
                ((e && e->message) ? e->message : "Unknown"));
            */
        }
        DEBUG("LibGit clone done");
        return LibGit(repo);
    }

    LibGit LibGit::open(std::string path) {
        git_repository* repo = nullptr;
        init();
        DEBUG("LibGit open start");
        if (git_repository_open(&repo, path.c_str()) < 0) {
            const git_error* e = git_error_last();
            auto repo = LibGit();
            return repo;
            /*throw std::runtime_error(std::string("Unable to open repository at " + path + " due to : ") +
                (e && e->message ? e->message : "Unknown"));
            */
        }

        DEBUG("LibGit open done");
        return LibGit(repo);
    }

    LibGit LibGit::openOrInit(std::string path) {
        git_repository* repo = nullptr;
        init();
        DEBUG("LibGit open or init start");
        if (git_repository_open(&repo, path.c_str()) < 0) {
            if (git_repository_init(&repo, path.c_str(), /*isbare*/0) < 0) {
                const git_error* e = git_error_last();

                throw std::runtime_error(std::string("Unable to open or init repository at " + path + " due to : ") +
                    (e && e->message ? e->message : "Unknown"));
            }
        }

        DEBUG("LibGit open done");
        return LibGit(repo);
    }
    bool LibGit::isPathIgnored(fs::path path) {
        return isPathIgnored(path.string());
    }

    bool LibGit::isPathIgnored(const std::string& path) {
        DEBUG_FULL("LibGit isPathIgnored");
        if (!isRepoValid) return false;
        int ignored;
        if (git_ignore_path_is_ignored(&ignored, repo.get(), path.c_str()) < 0) {
            return false;
        }
        return ignored == 1;
    }

    void LibGit::add(const std::string& path) {
        fs::path p(path);
        add(p);
    }

    void LibGit::add(const fs::path& path) {
        git_index* index = nullptr;

        if (!isRepoValid) {
            throw std::runtime_error("This is not a valid Git repo, unable to add");
        }

        fs::path relPath = fs::relative(path, root);

        DEBUG("LibGit add " << relPath.c_str());

        std::lock_guard<std::mutex> lock(gitMutex);
        int err = 0;
        err = git_repository_index(&index, repo.get());
        err = git_index_add_bypath(index, relPath.string().c_str());
        err = git_index_write(index);
        git_index_free(index);

        if (err < 0) {
            const git_error* e = git_error_last();
            throw std::runtime_error(std::string("Unable to add file ") + relPath.string().c_str() + " due to : " +
                (e && e->message ? e->message : "Unknown"));
        }
    };

    void LibGit::addAll() {
        if (!isRepoValid) {
            throw std::runtime_error("This is not a valid Git repo, unable to addAll");
        }
        DEBUG("LibGit add all - " << root);

        std::lock_guard<std::mutex> lock(gitMutex);
        int err = 0;
        git_index* index;
        err = git_repository_index(&index, repo.get());
        err = git_index_add_all(index, NULL, 0, NULL, NULL);
        git_index_free(index);
        if (err < 0) {
            throw std::runtime_error("Failed to add all files to index");
        }
    }

    void LibGit::addIgnoreRule(const std::string& rule) {
        if (!isRepoValid) {
            throw std::runtime_error("This is not a valid Git repo, unable to addIgnoreRule");
        }
        git_ignore_add_rule(repo.get(), rule.c_str());
    }

    void LibGit::checkout(const std::string& blobId, git_checkout_options opts) {
        if (!isRepoValid) {
            throw std::runtime_error("This is not a valid Git repo, unable to Checkout");
        }
        DEBUG("LibGit checkout - " << blobId);

        std::lock_guard<std::mutex> lock(gitMutex);
        git_object* target = nullptr;
        git_reference* ref = nullptr;

        int err = 0;

        if (git_reference_dwim(&ref, repo.get(), blobId.c_str()) == 0) {
            //It's a branch/tag

            const char* refname = git_reference_name(ref);
            DEBUG("LibGit checkout branch/tag ref - " << refname);
            err = git_repository_set_head(repo.get(), refname);
            err = git_checkout_head(repo.get(), &opts);
            git_reference_free(ref);
        }
        else {
            //It's a commit/tree

            DEBUG("LibGit checkout commit/tree target - " << blobId);
            err = git_revparse_single(&target, repo.get(), blobId.c_str());
            err = git_checkout_tree(repo.get(), target, &opts);
            err = git_repository_set_head_detached(repo.get(), git_object_id(target));
            git_object_free(target);
        }

        if (err < 0) {
            const git_error* e = git_error_last();
            throw std::runtime_error(std::string("Unable to checkout ") + blobId + " due to : " +
                (e && e->message ? e->message : "Unknown"));
        }
    }

    bool LibGit::branchExists(const std::string& name) {
        if (!isRepoValid) {
            false;
        }
        git_reference* ref = nullptr;
        bool exists = git_reference_dwim(&ref, repo.get(), name.c_str()) == 0;
        if (ref) {
            git_reference_free(ref);
        }
        return exists;
    }

    void LibGit::branchCreate(const std::string& name) {
        if (!isRepoValid) {
            throw std::runtime_error("This is not a valid Git repo, unable to createBranch");
        }
        DEBUG("LibGit branchCreate - " << name);
        std::lock_guard<std::mutex> lock(gitMutex);
        git_reference* ref = nullptr;
        git_object* head = nullptr;
        git_commit* commit = nullptr;

        int err = 0;
        err = git_revparse_single(&head, repo.get(), "HEAD");
        err = git_object_peel((git_object**)&commit, head, GIT_OBJECT_COMMIT);
        err = git_branch_create(&ref, repo.get(), name.c_str(), commit, 0);

        git_commit_free(commit);
        git_object_free(head);
        git_reference_free(ref);

        if (err < 0) {
            const git_error* e = git_error_last();
            throw std::runtime_error(std::string("Unable tocreate branch ") + name + " due to : " +
                (e && e->message ? e->message : "Unknown"));
        }
    }

    void LibGit::setSignature(const std::string& username, const std::string& email) {
        this->username = username;
        this->email = email;
        if (git_signature_new(&signature, username.c_str(), email.c_str(), time(nullptr), 0) < 0) {
            throw std::runtime_error("Failed to set signature");
        }
    }

    void LibGit::commit(std::string message) {
        git_index* index = nullptr;
        int err = 0;
        if (!isRepoValid) {
            throw std::runtime_error("This is not a valid Git repo, unable to commit");
        }
        std::lock_guard<std::mutex> lock(gitMutex);

        err = git_repository_index(&index, repo.get());
        err = git_index_read(index, 0);

        git_oid tree_id;
        git_tree* tree = nullptr;
        git_reference* ref = nullptr;
        git_commit* parent = nullptr;
        git_oid commit_id;
        err = git_index_write_tree(&tree_id, index);
        err = git_tree_lookup(&tree, repo.get(), &tree_id);
        err = git_repository_head(&ref, repo.get());
        err = git_commit_lookup(&parent, repo.get(), git_reference_target(ref));
        err = git_commit_create_v(&commit_id, repo.get(), "HEAD", signature, signature, NULL, message.c_str(), tree, parent ? 1 : 0, parent);
        git_tree_free(tree);
        git_index_free(index);
        git_commit_free(parent);
        git_reference_free(ref);

        if (err < 0) {
            const git_error* e = git_error_last();
            throw std::runtime_error(std::string("Unable to create commit - ") + message + " due to : " +
                (e && e->message ? e->message : "Unknown"));
        }
    }

    void LibGit::resetHead(git_reset_t opt) {
        if (!isRepoValid) {
            throw std::runtime_error("This is not a valid Git repo, unable to resetHead");
        }
        DEBUG("LibGit reset HEAD - " << opt);
        git_object* target = NULL;
        int err;
        err = git_revparse_single(&target, repo.get(), "HEAD");

        std::lock_guard<std::mutex> lock(gitMutex);
        git_checkout_options opts = GIT_CHECKOUT_OPTIONS_INIT;
        err = git_reset(repo.get(), target, opt, &opts);
        git_object_free(target);

        if (err < 0) {
            const git_error* e = git_error_last();
            throw std::runtime_error(std::string("Unable to reset HEAD - ") + " due to : " +
                (e && e->message ? e->message : "Unknown"));
        }
    }

    std::vector<LibGit::FileDiff> LibGit::diff() {
        return diff("HEAD", "");
    }

    std::vector<LibGit::FileDiff> LibGit::diff(std::string fromBlobId, std::string toBlobId,
        git_diff_options opts) {
        if (!isRepoValid) {
            throw std::runtime_error("This is not a valid Git repo, unable to diff");
        }
        DEBUG("LibGit diff " << fromBlobId << toBlobId);
        std::vector<FileDiff> result;

        git_object* from_obj = nullptr;
        git_object* to_obj = nullptr;
        git_tree* from_tree = nullptr;
        git_tree* to_tree = nullptr;
        git_diff* diff = nullptr;

        int err = 0;

        // Resolve FROM (can be any git object)
        err = git_revparse_single(&from_obj, repo.get(), fromBlobId.c_str());
        err = git_object_peel((git_object**)&from_tree, from_obj, GIT_OBJECT_TREE);

        // Resolve TO (can be any git object)
        if (toBlobId != "") {
            err = git_revparse_single(&to_obj, repo.get(), toBlobId.c_str());
            err = git_object_peel((git_object**)&to_tree, to_obj, GIT_OBJECT_TREE);
        }

        // Create diff
        if (toBlobId == "") {
            err = git_diff_tree_to_workdir_with_index(
                &diff,
                repo.get(),
                from_tree,
                &opts
            );
        }
        else {
            err = git_diff_tree_to_tree(
                &diff,
                repo.get(),
                from_tree,
                to_tree,
                &opts
            );
        }

        /*
        git_diff
        └── git_diff_delta (file metadata)
              └── git_patch (optional, content diff)
                    └── hunks
                          └── lines
        */

        // Iterate diff
        size_t deltaNum = git_diff_num_deltas(diff); // delta = one file change

        DEBUG_FULL("LibGit diff deltaNum - " << deltaNum);
        for (size_t i = 0; i < deltaNum; i++) {
            const git_diff_delta* delta = git_diff_get_delta(diff, i);
            FileDiff f;

            f.status = delta->status;
            f.flags = (git_diff_flag_t)delta->flags;
            // delta->nfiles; // number of files in this delta ?
            f.oldPath = delta->old_file.path;
            f.newPath = delta->new_file.path;

            git_patch* patch = nullptr;
            err = git_patch_from_diff(&patch, diff, i);

            size_t hunkNum = git_patch_num_hunks(patch);
            DEBUG_FULL("LibGit diff hunkNum - " << hunkNum);
            for (int j = 0; j < hunkNum; j++) {
                const git_diff_hunk* hunk;

                size_t lineNum = 0;
                err = git_patch_get_hunk(&hunk, &lineNum, patch, j);

                Hunk h;
                h.oldStartLine = hunk->old_start;
                h.oldLinesCount = hunk->old_lines;
                h.newStartLine = hunk->new_start;
                h.newLinesCount = hunk->new_lines;
                h.header = std::string(hunk->header);

                git_blame* oldBlame = nullptr;
                git_blame* newBlame = nullptr;

                if (!f.oldPath.empty()) {
                    DEBUG_FULL("LibGit diff blame " << f.oldPath);
                    git_blame_file(&oldBlame, repo.get(), f.oldPath.c_str(), nullptr);
                }

                if (!f.newPath.empty()) {
                    DEBUG_FULL("LibGit diff blame " << f.newPath);
                    git_blame_file(&newBlame, repo.get(), f.newPath.c_str(), nullptr);
                }

                DEBUG_FULL("LibGit diff lineNum - " << lineNum);
                for (int k = 0; k < lineNum; k++) {
                    const git_diff_line* line;
                    git_patch_get_line_in_hunk(&line, patch, j, k);
                    LineDiff l;
                    l.type = (git_diff_line_t)line->origin;
                    l.oldLineNo = line->old_lineno;
                    l.newLineNo = line->new_lineno;
                    l.fileOffset = line->content_offset;
                    l.cont = std::string(line->content, line->content_len);

                    if (line->origin == GIT_DIFF_LINE_ADDITION) {
                        // Blame for new file
                        if (newBlame) {
                            const git_blame_hunk* bh = git_blame_get_hunk_byline(newBlame, l.newLineNo);
                            if (bh) {
                                l.blameAuthor = bh->final_signature->name;
                                l.blameEmail = bh->final_signature->email;
                                l.blameCommit = git_oid_tostr_s(&bh->final_commit_id);
                            }
                        }
                    }
                    else if (line->origin == GIT_DIFF_LINE_DELETION) {
                        if (oldBlame) {
                            const git_blame_hunk* bh = git_blame_get_hunk_byline(oldBlame, l.oldLineNo);
                            if (bh) {
                                l.blameAuthor = bh->final_signature->name;
                                l.blameEmail = bh->final_signature->name;
                                l.blameCommit = git_oid_tostr_s(&bh->final_commit_id);
                            }
                        }
                    }
                    else if (line->origin == GIT_DIFF_LINE_CONTEXT) {
                        if (newBlame) {
                            const git_blame_hunk* bh = git_blame_get_hunk_byline(newBlame, l.newLineNo);
                            if (bh) {
                                l.blameAuthor = bh->final_signature->name;
                                l.blameEmail = bh->final_signature->name;
                                l.blameCommit = git_oid_tostr_s(&bh->final_commit_id);
                            }
                        }
                        else if (oldBlame) {
                            const git_blame_hunk* bh = git_blame_get_hunk_byline(oldBlame, l.oldLineNo);
                            if (bh) {
                                l.blameAuthor = bh->final_signature->name;
                                l.blameEmail = bh->final_signature->name;
                                l.blameCommit = git_oid_tostr_s(&bh->final_commit_id);
                            }
                        }
                    }

                    h.lineDiffs.push_back(l);
                    // git_line_free(line); // line is ref counted
                }
                f.hunks.push_back(h);
                git_blame_free(oldBlame);
                git_blame_free(newBlame);
                //git_hunk_free(hunk); //hunk is ref counted
            }

            result.push_back(f);
            git_patch_free(patch);
        }

        git_diff_free(diff);
        git_tree_free(from_tree);
        git_tree_free(to_tree);
        git_object_free(from_obj);
        git_object_free(to_obj);

        if (err < 0) {
            const git_error* e = git_error_last();
            throw std::runtime_error(
                std::string("Diff failed: ") +
                (e && e->message ? e->message : "Unknown")
            );
        }

        return result;
    }
} // namespace copypasta


