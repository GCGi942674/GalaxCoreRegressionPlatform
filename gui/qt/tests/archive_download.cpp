#include "ArchiveDownload.h"
#include "RerunTask.h"
#include <QCoreApplication>
#include <QTcpServer>
#include <QTcpSocket>
#include <QEventLoop>
#include <QTemporaryDir>
#include <QtEndian>
#include <cstdio>

bool scenario(bool disconnect,bool corrupt,bool badResume,bool large,bool legacy=false) {
    QTemporaryDir directory;if(!directory.isValid())return false;
    const auto destination=directory.path()+"/run";
    const uint64_t size=large?65ULL*1024*1024:512*1024;
    const QByteArray block(128*1024,'x');
    QCryptographicHash digest(QCryptographicHash::Sha256);
    for(uint64_t at=0;at<size;at+=uint64_t(block.size()))digest.addData(block.left(int(qMin<uint64_t>(block.size(),size-at))));
    const auto sha=QString::fromLatin1(digest.result().toHex());
    Json identity={{"task_id",1u},{"run_id","run-1"},{"name","evidence.run"}};
    auto metadata=identity;metadata["sha256"]=sha.toStdString();
    if(badResume)metadata["run_id"]="different-run";
    {QFile part(destination+".gcrp-part");if(!part.open(QIODevice::WriteOnly)||part.write(block)!=block.size())return false;}
    {QFile meta(destination+".gcrp-part.json");if(!meta.open(QIODevice::WriteOnly))return false;meta.write(QByteArray::fromStdString(metadata.dump()));}
    if(legacy)QFile::remove(destination+".gcrp-part");
    {QFile existing(destination);existing.open(QIODevice::WriteOnly);existing.write("keep existing destination");}
    QTcpServer server;if(!server.listen(QHostAddress::LocalHost,0))return false;
    QObject context;uint64_t firstOffset=0;int requests=0;
    QObject::connect(&server,&QTcpServer::newConnection,&context,[&]{
        auto socket=server.nextPendingConnection();auto buffer=std::make_shared<QByteArray>();
        QObject::connect(socket,&QTcpSocket::disconnected,socket,&QObject::deleteLater);
        QObject::connect(socket,&QTcpSocket::readyRead,&context,[&,socket,buffer]{
            buffer->append(socket->readAll());if(buffer->size()<4)return;
            const auto length=qFromBigEndian<quint32>(reinterpret_cast<const uchar*>(buffer->constData()));
            if(buffer->size()<int(length)+4)return;
            const auto q=Json::parse(buffer->constData()+4,buffer->constData()+4+length);
            const auto at=q.at("data").at("offset").get<uint64_t>();if(!requests++)firstOffset=at;
            if(disconnect){socket->disconnectFromHost();return;}
            auto bytes=block.left(int(qMin<uint64_t>(block.size(),size-at)));if(corrupt&&!bytes.isEmpty())bytes[0]='z';
            const auto body=Json{{"version",1},{"type","response"},{"request_id",q.at("request_id")},
                {"data",{{"code",0},{"result",{{"size",size},{"next_offset",at+uint64_t(bytes.size())},
                    {"complete",true},{"sha256",sha.toStdString()},{"data_hex",bytes.toHex().toStdString()}}}}}}.dump();
            QByteArray frame(4,'\0');qToBigEndian<quint32>(quint32(body.size()),reinterpret_cast<uchar*>(frame.data()));
            frame.append(body.data(),int(body.size()));socket->write(frame);
        });
    });
    RpcClient rpc;rpc.configure("127.0.0.1",server.serverPort(),1000);
    QEventLoop loop;bool done=false,ok=false;uint64_t maximum=0;
    auto transfer=new ArchiveDownload(rpc,identity,destination,legacy?QString{}:sha,
        [&](uint64_t at,uint64_t){maximum=at;},[&](bool success,QString){done=true;ok=success;loop.quit();},&context);
    QTimer::singleShot(0,transfer,[transfer]{transfer->start();});
    QTimer::singleShot(90000,&loop,&QEventLoop::quit);loop.exec();
    if(!done)return false;
    if(!badResume&&firstOffset!=(legacy?uint64_t(0):uint64_t(block.size())))return false;
    QFile result(destination);if(!result.open(QIODevice::ReadOnly))return false;
    if(disconnect||corrupt||badResume)return !ok&&result.readAll()=="keep existing destination"&&QFile::exists(destination+".gcrp-part");
    if(!ok||maximum!=size||uint64_t(result.size())!=size||QFile::exists(destination+".gcrp-part"))return false;
    QCryptographicHash actual(QCryptographicHash::Sha256);while(!result.atEnd())actual.addData(result.read(131072));
    return actual.result().toHex()==sha.toLatin1();
}
bool rerun() {
    const Json original={{"task_id",123u},{"task_type","REGRESSION"},{"status","FAILED"},{"target","device"},
        {"priority",7},{"revision","18954"},{"payload",{{"spec_version",2},{"case","case/run.tcl"},{"flow","route"},
            {"flow_config",{{"route_design",1}}},{"timeout_seconds",5400u},
            {"context",{{"source","scheduled"},{"date","2026-10-18"},{"batch_id","daily"},{"template","route"}}}}}};
    const auto copy=original;const auto task=rerunTask(original,"rerun-new",0);
    if(original!=copy||task["revision"]!=original["revision"]||task["priority"]!=7||
        task["payload"]["flow_config"]!=original["payload"]["flow_config"]||
        task["payload"]["context"]["source"]!="manual"||task["payload"]["context"].contains("date")||
        task["payload"]["context"]["suite"]!="rerun-task-123")return false;
    auto legacy=original;legacy["payload"].erase("flow_config");
    try{rerunTask(legacy,"rerun-new",1);return false;}catch(const std::exception&){}
    return rerunTask(original,"rerun-new",0)==task;
}
int main(int argc,char** argv){
    QCoreApplication app(argc,argv);
    const bool ok=rerun()&&scenario(true,false,false,false)&&scenario(false,true,false,false)&&
        scenario(false,false,true,false)&&scenario(false,false,false,true)&&scenario(false,false,false,false,true);
    std::fprintf(stderr,"QT_ARCHIVE_DOWNLOAD_AND_RERUN %s\n",ok?"PASS":"FAIL");return ok?0:1;
}
