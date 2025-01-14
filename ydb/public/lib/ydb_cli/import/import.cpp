#include "import.h"

#include <util/stream/format.h>
#include <ydb/public/sdk/cpp/client/ydb_driver/driver.h>
#include <ydb/public/sdk/cpp/client/ydb_operation/operation.h>
#include <ydb/public/sdk/cpp/client/ydb_proto/accessor.h>
#include <ydb/public/sdk/cpp/client/ydb_scheme/scheme.h>
#include <ydb/public/sdk/cpp/client/ydb_table/table.h>

#include <ydb/public/api/protos/ydb_formats.pb.h>
#include <ydb/public/api/protos/ydb_table.pb.h>
#include <ydb/public/lib/json_value/ydb_json_value.h>
#include <ydb/public/lib/ydb_cli/common/csv_parser.h>
#include <ydb/public/lib/ydb_cli/common/recursive_list.h>
#include <ydb/public/lib/ydb_cli/common/interactive.h>
#include <ydb/public/lib/ydb_cli/common/progress_bar.h>
#include <ydb/public/lib/ydb_cli/dump/util/util.h>
#include <ydb/public/lib/ydb_cli/import/cli_arrow_helpers.h>

#include <library/cpp/string_utils/csv/csv.h>
#include <library/cpp/threading/future/async.h>

#include <util/folder/path.h>
#include <util/generic/vector.h>
#include <util/stream/file.h>
#include <util/stream/length.h>
#include <util/string/builder.h>
#include <util/system/thread.h>

#include <contrib/libs/apache/arrow/cpp/src/arrow/api.h>
#include <contrib/libs/apache/arrow/cpp/src/arrow/io/api.h>
#include <contrib/libs/apache/arrow/cpp/src/arrow/ipc/api.h>
#include <contrib/libs/apache/arrow/cpp/src/arrow/result.h>
#include <contrib/libs/apache/arrow/cpp/src/parquet/arrow/reader.h>
#include <contrib/libs/apache/arrow/cpp/src/parquet/arrow/reader.h>
#include <contrib/libs/apache/arrow/cpp/src/parquet/file_reader.h>

#include <stack>

#if defined(_win32_)
#include <windows.h>
#include <io.h>
#elif defined(_unix_)
#include <unistd.h>
#endif


namespace NYdb {
namespace NConsoleClient {
namespace {

inline
TStatus MakeStatus(EStatus code = EStatus::SUCCESS, const TString& error = {}) {
    NYql::TIssues issues;
    if (error) {
        issues.AddIssue(NYql::TIssue(error));
    }
    return TStatus(code, std::move(issues));
}

TStatus WaitForQueue(const size_t maxQueueSize, std::vector<TAsyncStatus>& inFlightRequests) {
    while (!inFlightRequests.empty() && inFlightRequests.size() >= maxQueueSize) {
        NThreading::WaitAny(inFlightRequests).Wait();
        ui32 delta = 0;
        for (ui32 i = 0; i + delta < inFlightRequests.size();) {
            if (inFlightRequests[i].HasValue() || inFlightRequests[i].HasException()) {
                auto status = inFlightRequests[i].ExtractValueSync();
                if (!status.IsSuccess()) {
                    return status;
                }
                ++delta;
                inFlightRequests[i] = inFlightRequests[inFlightRequests.size() - delta];
            } else {
                ++i;
            }
        }
        inFlightRequests.resize(inFlightRequests.size() - delta);
    }

    return MakeStatus();
}

TString PrettifyBytes(double bytes) {
    return ToString(HumanReadableSize(bytes, SF_BYTES));
}

void InitCsvParser(TCsvParser& parser,
                   bool& removeLastDelimiter,
                   NCsvFormat::TLinesSplitter& csvSource,
                   const TImportFileSettings& settings,
                   const std::map<TString, TType>* columnTypes,
                   const NTable::TTableDescription* dbTableInfo) {
    if (settings.Header_ || settings.HeaderRow_) {
        TString headerRow;
        if (settings.Header_) {
            headerRow = csvSource.ConsumeLine();
        }
        if (settings.HeaderRow_) {
            headerRow = settings.HeaderRow_;
        }
        if (headerRow.EndsWith("\r\n")) {
            headerRow.erase(headerRow.size() - 2);
        }
        if (headerRow.EndsWith("\n")) {
            headerRow.erase(headerRow.size() - 1);
        }
        if (headerRow.EndsWith(settings.Delimiter_)) {
            removeLastDelimiter = true;
            headerRow.erase(headerRow.size() - settings.Delimiter_.size());
        }
        parser = TCsvParser(std::move(headerRow), settings.Delimiter_[0], settings.NullValue_, columnTypes);
    } else {
        TVector<TString> columns;
        Y_ENSURE_BT(dbTableInfo);
        for (const auto& column : dbTableInfo->GetColumns()) {
            columns.push_back(column.Name);
        }
        parser = TCsvParser(std::move(columns), settings.Delimiter_[0], settings.NullValue_, columnTypes);
    }
    parser.BuildLineType();
}

FHANDLE GetStdinFileno() {
#if defined(_win32_)
    return GetStdHandle(STD_INPUT_HANDLE);
#elif defined(_unix_)
    return STDIN_FILENO;
#endif
}

class TMaxInflightGetter {
public:
    TMaxInflightGetter(ui64 totalMaxInFlight, std::atomic<ui64>& currentFileCount)
        : TotalMaxInFlight(totalMaxInFlight)
        , CurrentFileCount(currentFileCount) {
    }

    ~TMaxInflightGetter() {
        --CurrentFileCount;
    }

    ui64 GetCurrentMaxInflight() const {
        return (TotalMaxInFlight - 1) / CurrentFileCount + 1; // round up
    }

private:
    ui64 TotalMaxInFlight;
    std::atomic<ui64>& CurrentFileCount;
};

class TCsvFileReader {
public:
    class TFileChunk {
    public:
        TFileChunk(TFile file, THolder<IInputStream>&& stream, ui64 size = std::numeric_limits<ui64>::max())
            : File(file)
            , Stream(std::move(stream))
            , CountStream(MakeHolder<TCountingInput>(Stream.Get()))
            , Size(size) {
        }

        bool ConsumeLine(TString& line) {
            ui64 prevCount = CountStream->Counter();
            line = NCsvFormat::TLinesSplitter(*CountStream).ConsumeLine();
            if (prevCount == CountStream->Counter() || prevCount >= Size) {
                return false;
            }
            return true;
        }

        ui64 GetReadCount() const {
            return CountStream->Counter();
        }

    private:
        TFile File;
        THolder<IInputStream> Stream;
        THolder<TCountingInput> CountStream;
        ui64 Size;
    };

public:
    TCsvFileReader(const TString& filePath, const TImportFileSettings& settings, TString& headerRow, ui64 maxThreads) {
        TFile file;
        if (filePath) {
            file = TFile(filePath, RdOnly);
        } else {
            file = TFile(GetStdinFileno());
        }
        auto input = MakeHolder<TFileInput>(file);
        TCountingInput countInput(input.Get());

        if (settings.Header_) {
            headerRow = NCsvFormat::TLinesSplitter(countInput).ConsumeLine();
        }
        for (ui32 i = 0; i < settings.SkipRows_; ++i) {
            NCsvFormat::TLinesSplitter(countInput).ConsumeLine();
        }
        i64 skipSize = countInput.Counter();

        i64 fileSize = file.GetLength();
        if (filePath.empty() || fileSize == -1) {
            SplitCount = 1;
            Chunks.emplace_back(file, std::move(input));
            return;
        }

        SplitCount = Min(maxThreads, (fileSize - skipSize) / settings.BytesPerRequest_ + 1);
        i64 chunkSize = (fileSize - skipSize) / SplitCount;
        if (chunkSize == 0) {
            SplitCount = 1;
            chunkSize = fileSize - skipSize;
        }

        i64 curPos = skipSize;
        i64 seekPos = skipSize;
        Chunks.reserve(SplitCount);
        TString temp;
        file = TFile(filePath, RdOnly);
        file.Seek(seekPos, sSet);
        THolder<TFileInput> stream = MakeHolder<TFileInput>(file);
        for (size_t i = 0; i < SplitCount; ++i) {
            seekPos += chunkSize;
            i64 nextPos = seekPos;
            auto nextFile = TFile(filePath, RdOnly);
            auto nextStream = MakeHolder<TFileInput>(nextFile);
            nextFile.Seek(seekPos, sSet);
            if (seekPos > 0) {
                nextFile.Seek(-1, sCur);
                nextPos += nextStream->ReadLine(temp);
            }

            Chunks.emplace_back(file, std::move(stream), nextPos - curPos);
            file = std::move(nextFile);
            stream = std::move(nextStream);
            curPos = nextPos;
        }
    }

    TFileChunk& GetChunk(size_t threadId) {
        if (threadId >= Chunks.size()) {
            throw yexception() << "File chunk number is too big";
        }
        return Chunks[threadId];
    }

    size_t GetThreadLimit(size_t thread_id) const {
        return MaxInFlight / SplitCount + (thread_id < MaxInFlight % SplitCount ? 1 : 0);
    }

    size_t GetSplitCount() const {
        return SplitCount;
    }

private:
    TVector<TFileChunk> Chunks;
    size_t SplitCount;
    size_t MaxInFlight;
};

class TJobInFlightManager {
public:
    TJobInFlightManager(size_t orderNum, size_t fileCount, size_t maxJobInFlight)
        : MaxJobInFlight(maxJobInFlight)
        , CurrentFileCount(fileCount)
        , CurrentSemaphoreMax(GetSemaphoreMaxValue(orderNum))
        , JobsSemaphore(GetSemaphoreMaxValue(orderNum)) {
    }

    // Return value: true if this call was useful (Inflight manager is still working)
    // Parameter informedSoFar: number of other Inflight managers already informed. Needed to calculate newSemaphoreMax
    bool OnAnotherFileFinished(size_t informedSoFar) {
        std::lock_guard<std::mutex> lock(Mutex);
        if (Finished || CurrentFileCount <= 1) {
            return false;
        }
        --CurrentFileCount;
        size_t newSemaphoreMax = GetSemaphoreMaxValue(informedSoFar);
        size_t semaphoreSizeDiff = newSemaphoreMax - CurrentSemaphoreMax;
        CurrentSemaphoreMax = newSemaphoreMax;
        if (semaphoreSizeDiff > 0) {
            JobsSemaphore.release(semaphoreSizeDiff);
        }
        return true;
    }

    void AcquireJob() {
        JobsSemaphore.acquire();
    }

    void ReleaseJob() {
        JobsSemaphore.release();
    }

    void Finish() {
        std::lock_guard<std::mutex> lock(Mutex);
        Finished = true;
    }

    void WaitForAllJobs() {
        std::lock_guard<std::mutex> lock(Mutex);
        for (size_t i = 0; i < CurrentSemaphoreMax; ++i) {
            JobsSemaphore.acquire();
        }
    }

private:
    size_t GetSemaphoreMaxValue(size_t orderNum) const {
        return Max(1ul, MaxJobInFlight / CurrentFileCount
            // One more thread for the first <MaxJobInFlight % CurrentFileCount> managers to utilize max jobs inflight
            + (orderNum < MaxJobInFlight % CurrentFileCount ? 1 : 0));
    }
    size_t MaxJobInFlight;
    size_t CurrentFileCount;
    size_t CurrentSemaphoreMax;
    bool Finished = false;
    std::mutex Mutex;
    // Jobs starts on launching a worker in a separate thread that builds TValue and sends request
    // Job ends on receiving final request (after all retries)
    std::counting_semaphore<> JobsSemaphore;
}; // TJobInflightManager

} // namespace

class TImportFileClient::TImpl {
public:
    explicit TImpl(const TDriver& driver, const TClientCommand::TConfig& rootConfig,
                                     const TImportFileSettings& settings);
    TStatus Import(const TVector<TString>& filePaths, const TString& dbPath);

private:
    using ProgressCallbackFunc = std::function<void (ui64, ui64)>;

    TStatus UpsertCsv(IInputStream& input,
                      const TString& dbPath,
                      const TString& filePath,
                      std::optional<ui64> inputSizeHint,
                      ProgressCallbackFunc & progressCallback,
                      std::shared_ptr<TJobInFlightManager> jobInflightManager);

    TStatus UpsertCsvByBlocks(const TString& filePath,
                              const TString& dbPath);
    TAsyncStatus UpsertTValueBuffer(const TString& dbPath, TValueBuilder& builder);
    TAsyncStatus UpsertTValueBuffer(const TString& dbPath, TValue&& rows);
    TStatus UpsertJson(IInputStream &input, const TString &dbPath, std::optional<ui64> inputSizeHint,
                       ProgressCallbackFunc & progressCallback);
    TStatus UpsertParquet(const TString& filename, const TString& dbPath, ProgressCallbackFunc & progressCallback);
    TAsyncStatus UpsertParquetBuffer(const TString& dbPath, const TString& buffer, const TString& strSchema);
    TType GetTableType();
    std::map<TString, TType> GetColumnTypes();
    void ValidateTValueUpsertTable();

    std::shared_ptr<NTable::TTableClient> TableClient;
    std::shared_ptr<NScheme::TSchemeClient> SchemeClient;
    const TImportFileSettings& Settings;
    NTable::TBulkUpsertSettings UpsertSettings;
    NTable::TRetryOperationSettings RetrySettings;
    std::unique_ptr<const NTable::TTableDescription> DbTableInfo;
    std::atomic<ui64> CurrentFileCount;
    std::atomic<ui64> TotalBytesRead = 0;
    // RequestInflight increases on sending a single request to server
    // Decreases on receiving any response for its request
    std::unique_ptr<std::counting_semaphore<>> RequestsInflight;
    // Common pool between all files for building TValues
    THolder<IThreadPool> ProcessingPool;
    std::atomic<bool> Failed = false;
    std::atomic<bool> InformedAboutLimit = false;
    THolder<TStatus> ErrorStatus;

    static constexpr ui32 VerboseModeStepSize = 1 << 27; // 128 MB
}; // TImpl

TImportFileClient::TImportFileClient(const TDriver& driver, const TClientCommand::TConfig& rootConfig,
                                     const TImportFileSettings& settings)
    : Impl_(new TImpl(driver, rootConfig, settings)) {
}

TStatus TImportFileClient::Import(const TVector<TString>& filePaths, const TString& dbPath) {
    return Impl_->Import(filePaths, dbPath);
}

TImportFileClient::TImpl::TImpl(const TDriver& driver, const TClientCommand::TConfig& rootConfig,
                                    const TImportFileSettings& settings)
    : TableClient(std::make_shared<NTable::TTableClient>(driver))
    , SchemeClient(std::make_shared<NScheme::TSchemeClient>(driver))
    , Settings(settings)
{
    RetrySettings
        .MaxRetries(TImportFileSettings::MaxRetries)
        .Idempotent(true)
        .Verbose(rootConfig.IsVerbose());
    IThreadPool::TParams poolParams;
    if (!Settings.NewlineDelimited_) {
        poolParams.SetBlocking(true);
    }
    ProcessingPool = CreateThreadPool(Settings.Threads_, 0, poolParams);
    RequestsInflight = std::make_unique<std::counting_semaphore<>>(Settings.MaxInFlightRequests_);
}

TStatus TImportFileClient::TImpl::Import(const TVector<TString>& filePaths, const TString& dbPath) {
    CurrentFileCount = filePaths.size();
    if (Settings.Format_ == EDataFormat::Tsv && Settings.Delimiter_ != "\t") {
        return MakeStatus(EStatus::BAD_REQUEST,
            TStringBuilder() << "Illegal delimiter for TSV format, only tab is allowed");
    }

    auto describeStatus = TableClient->RetryOperationSync(
        [this, dbPath](NTable::TSession session) {
            auto result = session.DescribeTable(dbPath).ExtractValueSync();
            if (result.IsSuccess()) {
                DbTableInfo = std::make_unique<const NTable::TTableDescription>(result.GetTableDescription());
            }
            return result;
        }, NTable::TRetryOperationSettings{RetrySettings}.MaxRetries(10));

    if (!describeStatus.IsSuccess()) {
        /// TODO: Remove this after server fix: https://github.com/ydb-platform/ydb/issues/7791
        if (describeStatus.GetStatus() == EStatus::SCHEME_ERROR) {
            auto describePathResult = NDump::DescribePath(*SchemeClient, dbPath);
            if (describePathResult.GetStatus() != EStatus::SUCCESS) {
                return MakeStatus(EStatus::SCHEME_ERROR,
                    TStringBuilder() << describePathResult.GetIssues().ToString() << dbPath);
            }
        }
        return describeStatus;
    }

    UpsertSettings
        .OperationTimeout(Settings.OperationTimeout_)
        .ClientTimeout(Settings.ClientTimeout_);

    bool isStdoutInteractive = IsStdoutInteractive();
    size_t filePathsSize = filePaths.size();
    std::mutex progressWriteLock;
    std::atomic<ui64> globalProgress{0};

    TProgressBar progressBar(100);

    auto writeProgress = [&]() {
        ui64 globalProgressValue = globalProgress.load();
        std::lock_guard<std::mutex> lock(progressWriteLock);
        progressBar.SetProcess(globalProgressValue / filePathsSize);
    };

    auto start = TInstant::Now();

    auto pool = CreateThreadPool(filePathsSize);
    TVector<NThreading::TFuture<TStatus>> asyncResults;

    // If the single empty filename passed, read from stdin, else from the file

    std::vector<std::shared_ptr<TJobInFlightManager>> inflightManagers;
    std::mutex inflightManagersLock;
    if ((Settings.Format_ == EDataFormat::Tsv || Settings.Format_ == EDataFormat::Csv) && !Settings.NewlineDelimited_) {
        inflightManagers.reserve(filePathsSize);
        // <maxInFlight> requests in flight on server and <maxThreads> threads building TValue
        size_t maxJobInflight = Settings.Threads_ + Settings.MaxInFlightRequests_;
        for (size_t i = 0; i < filePathsSize; ++i) {
            inflightManagers.push_back(std::make_shared<TJobInFlightManager>(i, filePathsSize, maxJobInflight));
        }
    }

    for (size_t fileOrderNumber = 0; fileOrderNumber < filePathsSize; ++fileOrderNumber) {
        const auto& filePath = filePaths[fileOrderNumber];
        auto func = [&, fileOrderNumber, this] {
            std::unique_ptr<TFileInput> fileInput;
            std::optional<ui64> fileSizeHint;

            if (!filePath.empty()) {
                const TFsPath dataFile(filePath);

                if (!dataFile.Exists()) {
                    return MakeStatus(EStatus::BAD_REQUEST,
                        TStringBuilder() << "File does not exist: " << filePath);
                }

                if (!dataFile.IsFile()) {
                    return MakeStatus(EStatus::BAD_REQUEST,
                        TStringBuilder() << "Not a file: " << filePath);
                }

                TFile file(filePath, OpenExisting | RdOnly | Seq);
                i64 fileLength = file.GetLength();
                if (fileLength && fileLength >= 0) {
                    fileSizeHint = fileLength;
                }

                fileInput = std::make_unique<TFileInput>(file, Settings.FileBufferSize_);
            }

            ProgressCallbackFunc progressCallback;

            if (isStdoutInteractive) {
                ui64 oldProgress = 0;
                progressCallback = [&, oldProgress](ui64 current, ui64 total) mutable {
                    ui64 progress = static_cast<ui64>((static_cast<double>(current) / total) * 100.0);
                    ui64 progressDiff = progress - oldProgress;
                    globalProgress.fetch_add(progressDiff);
                    oldProgress = progress;
                    writeProgress();
                };
            }

            IInputStream& input = fileInput ? *fileInput : Cin;

            try {
                switch (Settings.Format_) {
                    case EDataFormat::Default:
                    case EDataFormat::Csv:
                    case EDataFormat::Tsv:
                        if (Settings.NewlineDelimited_) {
                            return UpsertCsvByBlocks(filePath, dbPath);
                        } else {
                            auto status = UpsertCsv(input, dbPath, filePath, fileSizeHint, progressCallback,
                                inflightManagers.at(fileOrderNumber));
                            std::lock_guard<std::mutex> lock(inflightManagersLock);
                            inflightManagers[fileOrderNumber]->Finish();
                            size_t informedManagers = 0;
                            for (auto& inflightManager : inflightManagers) {
                                if (inflightManager->OnAnotherFileFinished(informedManagers)) {
                                    ++informedManagers;
                                }
                            }
                            return status;
                        }
                    case EDataFormat::Json:
                    case EDataFormat::JsonUnicode:
                    case EDataFormat::JsonBase64:
                        return UpsertJson(input, dbPath, fileSizeHint, progressCallback);
                    case EDataFormat::Parquet:
                        return UpsertParquet(filePath, dbPath, progressCallback);
                    default: ;
                }

                return MakeStatus(EStatus::BAD_REQUEST,
                            TStringBuilder() << "Unsupported format #" << (int) Settings.Format_);
            } catch (const std::exception& e) {
                return MakeStatus(EStatus::INTERNAL_ERROR,
                        TStringBuilder() << "Error: " << e.what());
            }
        };

        asyncResults.push_back(NThreading::Async(std::move(func), *pool));
    }

    NThreading::WaitAll(asyncResults).GetValueSync();
    for (const auto& asyncResult : asyncResults) {
        auto result = asyncResult.GetValueSync();
        if (!result.IsSuccess()) {
            return result;
        }
    }

    auto finish = TInstant::Now();
    auto duration = finish - start;
    progressBar.SetProcess(100);
    if (duration.SecondsFloat() > 0) {
        std::cerr << "Elapsed: " << std::setprecision(3) << duration.SecondsFloat() << " sec. Total read size: "
            << PrettifyBytes(TotalBytesRead) << ". Average processing speed: "
            << PrettifyBytes((double)TotalBytesRead / duration.SecondsFloat())  << "/s." << std::endl;
    }

    return MakeStatus(EStatus::SUCCESS);
}

inline
TAsyncStatus TImportFileClient::TImpl::UpsertTValueBuffer(const TString& dbPath, TValueBuilder& builder) {
    auto retryFunc = [this, dbPath, rows = builder.Build()]
            (NYdb::NTable::TTableClient& tableClient) mutable -> TAsyncStatus {
        NYdb::TValue rowsCopy(rows.GetType(), rows.GetProto());
        return tableClient.BulkUpsert(dbPath, std::move(rowsCopy), UpsertSettings)
            .Apply([](const NYdb::NTable::TAsyncBulkUpsertResult& bulkUpsertResult) {
                NYdb::TStatus status = bulkUpsertResult.GetValueSync();
                return NThreading::MakeFuture(status);
            });
    };
    return TableClient->RetryOperation(retryFunc, RetrySettings);
}

inline
TAsyncStatus TImportFileClient::TImpl::UpsertTValueBuffer(const TString& dbPath, TValue&& rows) {
    auto retryFunc = [this, dbPath, rows = std::move(rows)]
            (NYdb::NTable::TTableClient& tableClient) mutable -> TAsyncStatus {
        NYdb::TValue rowsCopy(rows.GetType(), rows.GetProto());
        return tableClient.BulkUpsert(dbPath, std::move(rowsCopy), UpsertSettings)
            .Apply([](const NYdb::NTable::TAsyncBulkUpsertResult& bulkUpsertResult) {
                NYdb::TStatus status = bulkUpsertResult.GetValueSync();
                return NThreading::MakeFuture(status);
            });
    };
    if (!RequestsInflight->try_acquire()) {
        if (Settings.Verbose_ && Settings.NewlineDelimited_) {
            if (!InformedAboutLimit.exchange(true)) {
                Cerr << (TStringBuilder() << "@ (each '@' means max request inflight is reached and a worker thread is waiting for "
                "any response from database)" << Endl);
            } else {
                Cerr << '@';
            }
        }
        RequestsInflight->acquire();
    }
    return TableClient->RetryOperation(retryFunc, RetrySettings)
        .Apply([this](const TAsyncStatus& asyncStatus) {
            NYdb::TStatus status = asyncStatus.GetValueSync();
            if (!status.IsSuccess()) {
                if (!Failed.exchange(true)) {
                    ErrorStatus = MakeHolder<TStatus>(status);
                }
            }
            RequestsInflight->release();
            return asyncStatus;
        });
}

TStatus TImportFileClient::TImpl::UpsertCsv(IInputStream& input,
                                     const TString& dbPath,
                                     const TString& filePath,
                                     std::optional<ui64> inputSizeHint,
                                     ProgressCallbackFunc & progressCallback,
                                     std::shared_ptr<TJobInFlightManager> jobInflightManager) {
    TCountingInput countInput(&input);
    NCsvFormat::TLinesSplitter splitter(countInput);

    auto columnTypes = GetColumnTypes();
    ValidateTValueUpsertTable();
    TInstant fileStartTime = TInstant::Now();

    TCsvParser parser;
    bool removeLastDelimiter = false;
    InitCsvParser(parser, removeLastDelimiter, splitter, Settings, &columnTypes, DbTableInfo.get());

    for (ui32 i = 0; i < Settings.SkipRows_; ++i) {
        splitter.ConsumeLine();
    }

    ui64 row = Settings.SkipRows_ + Settings.Header_ + 1;
    ui64 batchRows = 0;
    ui64 nextBorder = VerboseModeStepSize;
    ui64 batchBytes = 0;
    ui64 readBytes = 0;

    TString line;
    std::vector<TAsyncStatus> inFlightRequests;
    std::vector<TString> buffer;

    auto upsertCsvFunc = [&, jobInflightManager](std::vector<TString>&& buffer, ui64 row) {
        try {
            UpsertTValueBuffer(dbPath, parser.BuildList(buffer, filePath, row))
                .Apply([jobInflightManager](const TAsyncStatus& asyncStatus) {
                    jobInflightManager->ReleaseJob();
                    return asyncStatus;
                });
        } catch (const std::exception& e) {
            if (!Failed.exchange(true)) {
                ErrorStatus = MakeHolder<TStatus>(MakeStatus(EStatus::INTERNAL_ERROR, e.what()));
            }
            jobInflightManager->ReleaseJob();
        }
    };

    while (TString line = splitter.ConsumeLine()) {
        ++batchRows;
        if (line.empty()) {
            continue;
        }
        readBytes += line.size();
        batchBytes += line.size();

        if (removeLastDelimiter) {
            if (!line.EndsWith(Settings.Delimiter_)) {
                return MakeStatus(EStatus::BAD_REQUEST,
                        "According to the header, lines should end with a delimiter");
            }
            line.erase(line.size() - Settings.Delimiter_.size());
        }

        buffer.push_back(line);

        if (readBytes >= nextBorder && Settings.Verbose_) {
            nextBorder += VerboseModeStepSize;
            Cerr << (TStringBuilder() << "Processed " << PrettifyBytes(readBytes) << " and " << row + batchRows << " records" << Endl);
        }

        if (batchBytes < Settings.BytesPerRequest_) {
            continue;
        }

        if (inputSizeHint && progressCallback) {
            progressCallback(readBytes, *inputSizeHint);
        }

        auto workerFunc = [&upsertCsvFunc, row, buffer = std::move(buffer)]() mutable {
            upsertCsvFunc(std::move(buffer), row);
        };
        row += batchRows;
        batchRows = 0;
        batchBytes = 0;
        buffer.clear();

        jobInflightManager->AcquireJob();

        if (!ProcessingPool->AddFunc(workerFunc)) {
            return MakeStatus(EStatus::INTERNAL_ERROR, "Couldn't add worker func");
        }

        if (Failed) {
            break;
        }
    }

    // Send the rest if buffer is not empty
    if (!buffer.empty() && countInput.Counter() > 0 && !Failed) {
        jobInflightManager->AcquireJob();
        upsertCsvFunc(std::move(buffer), row);
    }

    jobInflightManager->WaitForAllJobs();

    TotalBytesRead += readBytes;

    if (Settings.Verbose_) {
        std::stringstream str;
        double fileProcessingTimeSeconds = (TInstant::Now() - fileStartTime).SecondsFloat();
        str << std::endl << "File " << filePath << " of " << PrettifyBytes(readBytes)
            << (Failed ? " failed in " : " processed in ") << std::setprecision(3) << fileProcessingTimeSeconds << " sec";
        if (fileProcessingTimeSeconds > 0) {
            str << ", " << PrettifyBytes((double)readBytes / fileProcessingTimeSeconds)  << "/s" << std::endl;
        }
        std::cerr << str.str();
    }
    if (Failed) {
        return *ErrorStatus;
    } else {
        return MakeStatus();
    }
}

TStatus TImportFileClient::TImpl::UpsertCsvByBlocks(const TString& filePath,
                                             const TString& dbPath) {
    TString headerRow;
    ui64 maxThreads = Max(1ul, Settings.Threads_ / CurrentFileCount);
    TCsvFileReader splitter(filePath, Settings, headerRow, maxThreads);
    ui64 threadCount = splitter.GetSplitCount();
    // MaxInFlightRequests_ requests in flight on server and threadCount threads building TValue
    size_t maxJobInflightTotal = threadCount + Settings.MaxInFlightRequests_;

    auto columnTypes = GetColumnTypes();
    ValidateTValueUpsertTable();

    TCsvParser parser;
    bool removeLastDelimiter = false;
    TStringInput headerInput(headerRow);
    NCsvFormat::TLinesSplitter headerSplitter(headerInput, Settings.Delimiter_[0]);
    InitCsvParser(parser, removeLastDelimiter, headerSplitter, Settings, &columnTypes, DbTableInfo.get());

    TVector<NThreading::TFuture<void>> threadResults;
    threadResults.reserve(threadCount);
    for (size_t threadId = 0; threadId < threadCount; ++threadId) {
        auto promise = NThreading::NewPromise<void>();
        threadResults.push_back(promise.GetFuture());
        auto workerFunc = [&, threadId, promise] () mutable {
            TInstant threadStartTime = TInstant::Now();
            size_t maxJobInflight = maxJobInflightTotal / threadCount + (threadId < maxJobInflightTotal % threadCount ? 1 : 0);
            // Jobs starts on starting building TValue and sends request
            // Job ends on receiving final request (after all retries)
            std::counting_semaphore<> jobsInflight(maxJobInflight);
            auto upsertCsvFunc = [&](std::vector<TString>&& buffer) {
                jobsInflight.acquire();
                try {
                    UpsertTValueBuffer(dbPath, parser.BuildList(buffer, filePath))
                        .Apply([&jobsInflight](const TAsyncStatus& asyncStatus) {
                            jobsInflight.release();
                            return asyncStatus;
                        });
                } catch (const std::exception& e) {
                    if (!Failed.exchange(true)) {
                        ErrorStatus = MakeHolder<TStatus>(MakeStatus(EStatus::INTERNAL_ERROR, e.what()));
                    }
                    jobsInflight.release();
                }
            };
            std::vector<TAsyncStatus> inFlightRequests;
            std::vector<TString> buffer;
            ui32 idx = Settings.SkipRows_;
            ui64 readBytes = 0;
            ui64 batchBytes = 0;
            ui64 nextBorder = VerboseModeStepSize;
            TString line;
            TCsvFileReader::TFileChunk& chunk = splitter.GetChunk(threadId);
            while (chunk.ConsumeLine(line)) {
                if (line.empty()) {
                    continue;
                }
                readBytes += line.size();
                batchBytes += line.size();
                if (removeLastDelimiter) {
                    if (!line.EndsWith(Settings.Delimiter_)) {
                        if (!Failed.exchange(true)) {
                            ErrorStatus = MakeHolder<TStatus>(MakeStatus(EStatus::BAD_REQUEST,
                                "According to the header, lines should end with a delimiter"));
                        }
                        break;
                    }
                    line.erase(line.size() - Settings.Delimiter_.size());
                }
                buffer.push_back(line);
                ++idx;
                if (readBytes >= nextBorder && Settings.Verbose_) {
                    nextBorder += VerboseModeStepSize;
                    Cerr << (TStringBuilder() << "Processed " << PrettifyBytes(readBytes) << " and "
                        << idx << " records" << Endl);
                }
                if (batchBytes >= Settings.BytesPerRequest_) {
                    batchBytes = 0;
                    upsertCsvFunc(std::move(buffer));
                    buffer.clear();

                    if (Failed) {
                        break;
                    }
                }
            }

            if (!buffer.empty() && chunk.GetReadCount() != 0 && !Failed) {
                upsertCsvFunc(std::move(buffer));
            }

            // Wait for all jobs for current thread to finish
            for (size_t i = 0; i < maxJobInflight; ++i) {
                jobsInflight.acquire();
            }

            TotalBytesRead += readBytes;

            if (Settings.Verbose_) {
                std::stringstream str;
                double threadProcessingTimeSeconds = (TInstant::Now() - threadStartTime).SecondsFloat();
                str << std::endl << "File " << filePath << ", thread " << threadId << " processed " << PrettifyBytes(readBytes) << " and "
                    << (Failed ? "failed in " : "successfully finished in ") << std::setprecision(3)
                    << threadProcessingTimeSeconds << " sec";
                if (threadProcessingTimeSeconds > 0) {
                    str << ", " << PrettifyBytes((double)readBytes / threadProcessingTimeSeconds)  << "/s" << std::endl;
                }
                std::cerr << str.str();
            }
            promise.SetValue();
        };

        if (!ProcessingPool->AddFunc(workerFunc)) {
            return MakeStatus(EStatus::INTERNAL_ERROR, "Couldn't add worker func");
        }
    }
    NThreading::WaitAll(threadResults).Wait();
    if (Failed) {
        return *ErrorStatus;
    }
    return MakeStatus();
}

TStatus TImportFileClient::TImpl::UpsertJson(IInputStream& input, const TString& dbPath, std::optional<ui64> inputSizeHint,
                                      ProgressCallbackFunc & progressCallback) {
    const TType tableType = GetTableType();
    ValidateTValueUpsertTable();

    TMaxInflightGetter inFlightGetter(Settings.MaxInFlightRequests_, CurrentFileCount);

    ui64 readBytes = 0;
    ui64 batchBytes = 0;

    TString line;
    std::vector<TString> batchLines;
    std::vector<TAsyncStatus> inFlightRequests;

    auto upsertJson = [&](const std::vector<TString>& batchLines) {
        TValueBuilder batch;
        batch.BeginList();

        for (auto &line : batchLines) {
            batch.AddListItem(JsonToYdbValue(line, tableType, Settings.BinaryStringsEncoding_));
        }

        batch.EndList();

        auto value = UpsertTValueBuffer(dbPath, batch);
        return value.ExtractValueSync();
    };

    while (size_t size = input.ReadLine(line)) {
        batchLines.push_back(line);
        batchBytes += size;
        readBytes += size;

        if (inputSizeHint && progressCallback) {
            progressCallback(readBytes, *inputSizeHint);
        }

        if (batchBytes < Settings.BytesPerRequest_) {
            continue;
        }

        batchBytes = 0;

        auto asyncUpsertJson = [&, batchLines = std::move(batchLines)]() {
            return upsertJson(batchLines);
        };

        batchLines.clear();

        inFlightRequests.push_back(NThreading::Async(asyncUpsertJson, *ProcessingPool));

        auto status = WaitForQueue(inFlightGetter.GetCurrentMaxInflight(), inFlightRequests);
        if (!status.IsSuccess()) {
            return status;
        }
    }

    if (!batchLines.empty()) {
        upsertJson(std::move(batchLines));
    }

    return WaitForQueue(0, inFlightRequests);
}

TStatus TImportFileClient::TImpl::UpsertParquet([[maybe_unused]] const TString& filename,
                                         [[maybe_unused]] const TString& dbPath,
                                         [[maybe_unused]] ProgressCallbackFunc & progressCallback) {
#if defined(_WIN64) || defined(_WIN32) || defined(__WIN32__)
    return MakeStatus(EStatus::BAD_REQUEST, TStringBuilder() << "Not supported on Windows");
#else
    std::shared_ptr<arrow::io::ReadableFile> infile;
    arrow::Result<std::shared_ptr<arrow::io::ReadableFile>> fileResult = arrow::io::ReadableFile::Open(filename);
    if (!fileResult.ok()) {
        return MakeStatus(EStatus::BAD_REQUEST, TStringBuilder() << "Unable to open parquet file:" << fileResult.status().ToString());
    }
    std::shared_ptr<arrow::io::ReadableFile> readableFile = *fileResult;

    std::unique_ptr<parquet::arrow::FileReader> fileReader;

    arrow::Status st;
    st = parquet::arrow::OpenFile(readableFile, arrow::default_memory_pool(), &fileReader);
    if (!st.ok()) {
        return MakeStatus(EStatus::BAD_REQUEST, TStringBuilder() << "Error while initializing arrow FileReader: " << st.ToString());
    }

    auto metadata = parquet::ReadMetaData(readableFile);
    const i64 numRows = metadata->num_rows();
    const i64 numRowGroups = metadata->num_row_groups();

    std::vector<int> row_group_indices(numRowGroups);
    for (i64 i = 0; i < numRowGroups; i++) {
        row_group_indices[i] = i;
    }

    std::unique_ptr<arrow::RecordBatchReader> reader;

    st = fileReader->GetRecordBatchReader(row_group_indices, &reader);
    if (!st.ok()) {
        return MakeStatus(EStatus::BAD_REQUEST, TStringBuilder() << "Error while getting RecordBatchReader: " << st.ToString());
    }

    std::atomic<ui64> uploadedRows = 0;
    auto uploadedRowsCallback = [&](ui64 rows) {
        ui64 uploadedRowsValue = uploadedRows.fetch_add(rows);

        if (progressCallback) {
            progressCallback(uploadedRowsValue + rows, numRows);
        }
    };

    std::vector<TAsyncStatus> inFlightRequests;

    while (true) {
        std::shared_ptr<arrow::RecordBatch> batch;

        st = reader->ReadNext(&batch);
        if (!st.ok()) {
            return MakeStatus(EStatus::BAD_REQUEST, TStringBuilder() << "Error while reading next RecordBatch" << st.ToString());
        }

        // The read function will return null at the end of data stream.
        if (!batch) {
            break;
        }

        auto upsertParquetBatch = [&, batch = std::move(batch)]() {
            const TString strSchema = NYdb_cli::NArrow::SerializeSchema(*batch->schema());
            const size_t totalSize = NYdb_cli::NArrow::GetBatchDataSize(batch);
            const size_t sliceCount =
                (totalSize / (size_t)Settings.BytesPerRequest_) + (totalSize % Settings.BytesPerRequest_ != 0 ? 1 : 0);
            const i64 rowsInSlice = batch->num_rows() / sliceCount;

            for (i64 currentRow = 0; currentRow < batch->num_rows(); currentRow += rowsInSlice) {
                std::stack<std::shared_ptr<arrow::RecordBatch>> rowsToSendBatches;

                if (currentRow + rowsInSlice < batch->num_rows()) {
                    rowsToSendBatches.push(batch->Slice(currentRow, rowsInSlice));
                } else {
                    rowsToSendBatches.push(batch->Slice(currentRow));
                }

                do {
                    auto rowsBatch = std::move(rowsToSendBatches.top());
                    rowsToSendBatches.pop();

                    // Nothing to send. Continue.
                    if (rowsBatch->num_rows() == 0) {
                        continue;
                    }

                    // Logarithmic approach to find number of rows fit into the byte limit.
                    if (rowsBatch->num_rows() == 1 || NYdb_cli::NArrow::GetBatchDataSize(rowsBatch) < Settings.BytesPerRequest_) {
                        // Single row or fits into the byte limit.
                        auto value = UpsertParquetBuffer(dbPath, NYdb_cli::NArrow::SerializeBatchNoCompression(rowsBatch), strSchema);
                        auto status = value.ExtractValueSync();
                        if (!status.IsSuccess())
                            return status;

                        uploadedRowsCallback(rowsBatch->num_rows());
                    } else {
                        // Split current slice.
                        i64 halfLen = rowsBatch->num_rows() / 2;
                        rowsToSendBatches.push(rowsBatch->Slice(halfLen));
                        rowsToSendBatches.push(rowsBatch->Slice(0, halfLen));
                    }
                } while (!rowsToSendBatches.empty());
            };

            return MakeStatus();
        };

        inFlightRequests.push_back(NThreading::Async(upsertParquetBatch, *ProcessingPool));

        auto status = WaitForQueue(Settings.MaxInFlightRequests_, inFlightRequests);
        if (!status.IsSuccess()) {
            return status;
        }
    }

    return WaitForQueue(0, inFlightRequests);
#endif
}

inline
TAsyncStatus TImportFileClient::TImpl::UpsertParquetBuffer(const TString& dbPath, const TString& buffer, const TString& strSchema) {
    auto upsert = [this, dbPath, buffer, strSchema](NYdb::NTable::TTableClient& tableClient) -> TAsyncStatus {
        return tableClient.BulkUpsert(dbPath, NTable::EDataFormat::ApacheArrow, buffer, strSchema, UpsertSettings)
            .Apply([](const NYdb::NTable::TAsyncBulkUpsertResult& bulkUpsertResult) {
                NYdb::TStatus status = bulkUpsertResult.GetValueSync();
                return NThreading::MakeFuture(status);
            });
        };
    return TableClient->RetryOperation(upsert, RetrySettings);
}

TType TImportFileClient::TImpl::GetTableType() {
    TTypeBuilder typeBuilder;
    typeBuilder.BeginStruct();
    Y_ENSURE_BT(DbTableInfo);
    const auto& columns = DbTableInfo->GetTableColumns();
    for (auto it = columns.begin(); it != columns.end(); it++) {
        typeBuilder.AddMember((*it).Name, (*it).Type);
    }
    typeBuilder.EndStruct();
    return typeBuilder.Build();
}

std::map<TString, TType> TImportFileClient::TImpl::GetColumnTypes() {
    std::map<TString, TType> columnTypes;
    Y_ENSURE_BT(DbTableInfo);
    const auto& columns = DbTableInfo->GetTableColumns();
    for (auto it = columns.begin(); it != columns.end(); it++) {
        columnTypes.insert({(*it).Name, (*it).Type});
    }
    return columnTypes;
}

void TImportFileClient::TImpl::ValidateTValueUpsertTable() {
    auto columnTypes = GetColumnTypes();
    bool hasPgType = false;
    for (const auto& [_, type] : columnTypes) {
        if (TTypeParser(type).GetKind() == TTypeParser::ETypeKind::Pg) {
            hasPgType = true;
            break;
        }
    }
    Y_ENSURE_BT(DbTableInfo);
    if (DbTableInfo->GetStoreType() == NTable::EStoreType::Column && hasPgType) {
        throw TMisuseException() << "Import into column table with Pg type columns in not supported";
    }
}

}
}
