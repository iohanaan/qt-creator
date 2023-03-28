#include <QFuture>
#include <QPromise>
    const QList<QStringView> lines = patch.split(newLine);
    QList<int> startingPositions; // store starting positions of @@
static void readDiffPatch(QPromise<QList<FileData>> &promise, QStringView patch)
            if (promise.isCanceled())
                return;
                const FileData fileData = readDiffHeaderAndChunks(headerAndChunks, &readOk);
            const FileData fileData = readDiffHeaderAndChunks(headerAndChunks, &readOk);
        return;
    promise.addResult(fileDataList);
static void readGitPatch(QPromise<QList<FileData>> &promise, QStringView patch)
    QList<int> startingPositions; // store starting positions of git headers
    QList<PatchInfo> patches;
        if (promise.isCanceled())
            return;
        if (!detectFileData(fileDiff, &fileData, &remainingFileDiff))
            return;
    if (patches.isEmpty())
        return;
    promise.setProgressRange(0, patches.size());
        if (promise.isCanceled())
            return;
        promise.setProgressValue(i++);
        bool readOk = false;
            return;
    promise.addResult(fileDataList);
}
std::optional<QList<FileData>> DiffUtils::readPatch(const QString &patch)
{
    QPromise<QList<FileData>> promise;
    promise.start();
    readPatchWithPromise(promise, patch);
    if (promise.future().resultCount() == 0)
    return promise.future().result();
void DiffUtils::readPatchWithPromise(QPromise<QList<FileData>> &promise, const QString &patch)
    promise.setProgressRange(0, 1);
    promise.setProgressValue(0);
    readGitPatch(promise, croppedPatch);
    if (promise.future().resultCount() == 0)
        readDiffPatch(promise, croppedPatch);