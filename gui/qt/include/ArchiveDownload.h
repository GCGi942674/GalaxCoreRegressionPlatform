#pragma once
#include "RpcClient.h"
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QPointer>
#include <QSaveFile>
#include <QTimer>
#include <memory>
#include <stdexcept>

// Disk-backed resume for verified archives. Legacy unverified files require a
// fresh transfer and an explicit caller warning; neither mode buffers the file.
class ArchiveDownload : public QObject {
public:
    using Done=std::function<void(bool,QString)>;
    ArchiveDownload(RpcClient& rpc,Json identity,QString destination,QString sha256,
                    std::function<void(uint64_t,uint64_t)> progress,Done done,QObject* parent=nullptr)
        :QObject(parent),rpc_(rpc),identity_(std::move(identity)),destination_(std::move(destination)),
         digest_(std::move(sha256)),progress_(std::move(progress)),done_(std::move(done)),
         hash_(QCryptographicHash::Sha256),part_(destination_+".gcrp-part"),lock_(destination_+".gcrp-part.lock"){}
    void start(){
        if(!digest_.isEmpty()&&digest_.size()!=64){finish(false,QStringLiteral("Server SHA-256 收据无效。"));return;}
        lock_.setStaleLockTime(0);
        if(!lock_.tryLock(0)){finish(false,QStringLiteral("该下载已有其他进程操作。"));return;}
        if(QFileInfo(part_.fileName()).isSymLink()||QFileInfo(metaPath()).isSymLink()){
            finish(false,QStringLiteral("拒绝符号链接下载临时文件。"));return;
        }
        if(digest_.isEmpty()&&QFile::exists(part_.fileName())){
            finish(false,QStringLiteral("旧版归档无 Server 校验收据，不能安全续传；临时文件保留，请另选目标文件完整下载。"));return;
        }
        const auto fingerprint=metadata();
        if(QFile::exists(part_.fileName())){
            QFile meta(metaPath());
            try{
                if(!meta.open(QIODevice::ReadOnly)||meta.size()>4096||
                    Json::parse(meta.readAll().toStdString())!=fingerprint)
                    throw std::runtime_error("resume metadata does not match task/run/digest");
            }catch(const std::exception& e){finish(false,QString::fromUtf8(e.what())+QStringLiteral("；保留临时文件供检查。"));return;}
        }else{
            QSaveFile meta(metaPath());const auto bytes=QByteArray::fromStdString(fingerprint.dump());
            if(!meta.open(QIODevice::WriteOnly)||meta.write(bytes)!=bytes.size()||!meta.commit()){
                finish(false,QStringLiteral("无法保存下载续传信息。"));return;
            }
        }
        if(!part_.open(QIODevice::ReadWrite)||part_.size()<0||uint64_t(part_.size())>maxBytes()){
            finish(false,QStringLiteral("无法打开下载临时文件，或超过 1 GiB 归档上限。"));return;
        }
        prefix();
    }
    static uint64_t maxBytes(){return 1024ULL*1024*1024;}
private:
    RpcClient& rpc_;Json identity_;QString destination_,digest_;
    std::function<void(uint64_t,uint64_t)> progress_;Done done_;
    QCryptographicHash hash_;QFile part_;QLockFile lock_;
    std::unique_ptr<QSaveFile> target_;uint64_t position_{0},total_{0};bool haveTotal_{false},finished_{false};
    QString metaPath() const{return destination_+".gcrp-part.json";}
    Json metadata() const {auto m=identity_;m["sha256"]=digest_.toStdString();return m;}
    void finish(bool ok,const QString& message){
        if(finished_)return;finished_=true;part_.close();if(target_)target_->cancelWriting();lock_.unlock();
        done_(ok,message);deleteLater();
    }
    void later(void(ArchiveDownload::*method)()){
        QPointer<ArchiveDownload> guard(this);QTimer::singleShot(0,this,[guard,method]{if(guard)(guard.data()->*method)();});
    }
    void prefix(){
        // Rehash the actual persisted prefix; never trust an old offset alone.
        auto bytes=part_.read(128*1024);
        if(bytes.isEmpty()&&!part_.atEnd()){finish(false,part_.errorString());return;}
        hash_.addData(bytes);position_+=uint64_t(bytes.size());
        if(!part_.atEnd()){later(&ArchiveDownload::prefix);return;}
        request();
    }
    void request(){
        auto query=identity_;query["offset"]=position_;query["limit"]=131072;
        QPointer<ArchiveDownload> guard(this);
        rpc_.request("read_log",query,[guard](RpcReply reply){if(guard)guard->receive(std::move(reply));});
    }
    void receive(RpcReply reply){
        if(!reply.ok){
            finish(false,reply.error+(digest_.isEmpty()?QStringLiteral("；未校验的临时文件保留，请另选目标重新完整下载。"):
                QStringLiteral("；下次选择相同目标文件可继续下载。")));return;
        }
        try{
            const auto size=reply.result.at("size").get<uint64_t>(),end=reply.result.at("next_offset").get<uint64_t>();
            const auto hex=reply.result.at("data_hex").get<std::string>();
            if(hex.size()>262144||hex.size()%2||hex.find_first_not_of("0123456789abcdefABCDEF")!=std::string::npos)
                throw std::runtime_error("invalid archive bytes");
            const auto bytes=QByteArray::fromHex(QByteArray::fromStdString(hex));
            if(!reply.result.value("complete",false)||(!digest_.isEmpty()&&reply.result.value("sha256",std::string())!=digest_.toStdString())||
                size>maxBytes()||(haveTotal_&&size!=total_)||end<position_||end>size||
                end-position_!=uint64_t(bytes.size())||(end==position_&&end<size))
                throw std::runtime_error("archive changed, incomplete or outside 1 GiB bound");
            if(part_.write(bytes)!=bytes.size()||!part_.flush())throw std::runtime_error("cannot persist download");
            hash_.addData(bytes);position_=end;total_=size;haveTotal_=true;progress_(position_,total_);
            if(end<size){later(&ArchiveDownload::request);return;}
            if(!digest_.isEmpty()&&hash_.result().toHex()!=digest_.toLatin1())throw std::runtime_error("archive SHA-256 mismatch; destination not replaced");
            if(!part_.seek(0))throw std::runtime_error("cannot rewind verified download");
            target_.reset(new QSaveFile(destination_));
            if(!target_->open(QIODevice::WriteOnly))throw std::runtime_error("cannot open final destination");
            later(&ArchiveDownload::commit);
        }catch(const std::exception& e){finish(false,QString::fromUtf8(e.what())+QStringLiteral("；临时文件已保留。"));}
    }
    void commit(){
        const auto bytes=part_.read(128*1024);
        if((bytes.isEmpty()&&!part_.atEnd())||target_->write(bytes)!=bytes.size()){
            finish(false,QStringLiteral("写入目标文件失败；续传文件保留。"));return;
        }
        if(!part_.atEnd()){later(&ArchiveDownload::commit);return;}
        if(!target_->commit()){finish(false,target_->errorString());return;}
        target_.reset();part_.close();
        // Exact generated paths owned by this successful download.
        QFile::remove(part_.fileName());QFile::remove(metaPath());
        finish(true,(digest_.isEmpty()?QStringLiteral("已保存 %1 字节；旧版证据无 Server 校验收据，不代表 SHA-256 校验通过。"):
            QStringLiteral("已保存 %1 字节，SHA-256 校验通过。")).arg(qulonglong(total_)));
    }
};
