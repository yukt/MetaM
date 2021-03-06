#include "MetaMinimac.h"
#include "MarkovModel.h"
#include <iostream>
#include <iomanip>
#include <sstream>
#include "simplex.h"
#define RECOM_MIN 1e-04

using BT::Simplex;

using namespace std;

const int MAXBP = 999999999;

String MetaMinimac::Analyze()
{
    if(!myUserVariables.CheckValidity()) return "Command.Line.Error";


    cout<<" ------------------------------------------------------------------------------"<<endl;
    cout<<"                             INPUT VCF DOSAGE FILE                             "<<endl;
    cout<<" ------------------------------------------------------------------------------"<<endl;

    if (!ParseInputVCFFiles())
    {
        cout << "\n Program Exiting ... \n\n";
        return "Command.Line.Error";
    }

    if (!CheckSampleNameCompatibility())
    {
        cout << "\n Program Exiting ... \n\n";
        return "Input.VCF.Dose.Error";
    }

    if (!LoadEmpVariantInfo())
    {
        cout << "\n Program Exiting ... \n\n";
        return "Input.VCF.Dose.Error";
    }

    if (!OpenStreamOutputDosageFiles())
    {
        cout <<" Please check your write permissions in the output directory\n OR maybe the output directory does NOT exist ...\n";
        cout << "\n Program Exiting ... \n\n";
        return "File.Write.Error";
    }

    return PerformFinalAnalysis();
}

bool MetaMinimac::ParseInputVCFFiles()
{

    InPrefixList.clear();
    size_t pos = 0;
    std::string delimiter(myUserVariables.FileDelimiter) ;
    std::string token;
    int Count=0;
    string tempName=myUserVariables.inputFiles.c_str();
    while ((pos = tempName.find(delimiter)) != std::string::npos)
    {
        token = tempName.substr(0, pos);
        InPrefixList.push_back(token.c_str());
        tempName.erase(0, pos + delimiter.length());
        Count++;
    }
    InPrefixList.push_back(tempName.c_str());


    NoInPrefix=(int)InPrefixList.size();
    InputData.clear();
    InputData.resize(NoInPrefix);

    cout<<endl<<  " Number of Studies : "<<NoInPrefix<<endl;
    for(int i=0;i<NoInPrefix;i++)
    {
        cout<<  " -- Study "<<i+1<<" Prefix : "<<InPrefixList[i]<<endl;
    }


    if(NoInPrefix<2)
    {
        cout<<"\n ERROR ! Must have at least 2 studies for meta-imputation to work !!! "<<endl;
        cout<<" Program aborting ... "<<endl<<endl;
        return false;
    }
    if(NoInPrefix>4)
    {
        cout<<"\n ERROR ! Must have less than 5 studies for meta-imputation to work !!! "<<endl;
        cout<<" Program aborting ... "<<endl<<endl;
        return false;
    }

    return true;
}

bool MetaMinimac::CheckSampleNameCompatibility()
{
    cout<<"\n Checking Sample Compatibility across files ... "<<endl;
    for(int i=0;i<NoInPrefix;i++)
    {
        if(!InputData[i].LoadSampleNames(InPrefixList[i].c_str()))
            return false;
        if(i>0)
            if(!InputData[i].CheckSampleConsistency(InputData[i-1].numSamples,
                                                    InputData[i-1].individualName,
                                                    InputData[i-1].SampleNoHaplotypes,
                                                    InputData[i-1].DoseFileName,
                                                    InputData[i].DoseFileName))
                return false;

    }

    NoHaplotypes = InputData[0].numActualHaps;
    NoSamples = InputData[0].numSamples;
    cout<<" -- Found "<< NoSamples << " samples (" << NoHaplotypes << " haplotypes)." <<endl;

    if (myUserVariables.VcfBuffer > NoSamples)
        myUserVariables.VcfBuffer = NoSamples;
    return true;
}


void MetaMinimac::OpenStreamInputDosageFiles(bool siteOnly)
{
    InputDosageStream.resize(NoInPrefix);
    CurrentRecordFromStudy.resize(NoInPrefix);
    StudiesHasVariant.resize(NoInPrefix);
    for(int i=0; i<NoInPrefix;i++)
    {
        VcfHeader header;
        InputDosageStream[i] = new VcfFileReader();
        CurrentRecordFromStudy[i]= new VcfRecord();
        InputDosageStream[i]->open( (GetDosageFileFullName(InPrefixList[i])).c_str() , header);
        InputDosageStream[i]->setSiteOnly(siteOnly);
        InputDosageStream[i]->readRecord(*CurrentRecordFromStudy[i]);
        InputData[i].noMarkers = 0;
        InputData[i].noTypedMarkers = 0;
    }
    finChromosome = CurrentRecordFromStudy[0]->getChromStr();
}

void MetaMinimac::CloseStreamInputDosageFiles()
{
    for (int i = 0; i < NoInPrefix; i++)
    {
        delete InputDosageStream[i];
        delete CurrentRecordFromStudy[i];
    }
}

bool MetaMinimac::OpenStreamOutputDosageFiles()
{
    vcfdosepartial = ifopen(myUserVariables.outfile + ".metaDose.vcf"+(myUserVariables.gzip ? ".gz" : ""), "wb", myUserVariables.gzip ? InputFile::BGZF : InputFile::UNCOMPRESSED);
    VcfPrintStringPointer = (char*)malloc(sizeof(char) * (myUserVariables.PrintBuffer));
    if(vcfdosepartial==NULL)
    {
        cout <<"\n\n ERROR !!! \n Could NOT create the following file : "<< myUserVariables.outfile + ".metaDose.vcf"+(myUserVariables.gzip ? ".gz" : "") <<endl;
        return false;
    }
    ifprintf(vcfdosepartial,"##fileformat=VCFv4.1\n");
    time_t t = time(0);
    struct tm * now = localtime( & t );
    ifprintf(vcfdosepartial,"##filedate=%d.%d.%d\n",(now->tm_year + 1900),(now->tm_mon + 1) ,now->tm_mday);
    ifprintf(vcfdosepartial,"##source=MetaMinimac2.v%s\n",VERSION);
    ifprintf(vcfdosepartial,"##contig=<ID=%s>\n", finChromosome.c_str());
    ifprintf(vcfdosepartial,"##INFO=<ID=AF,Number=1,Type=Float,Description=\"Estimated Alternate Allele Frequency\">\n");
    ifprintf(vcfdosepartial,"##INFO=<ID=MAF,Number=1,Type=Float,Description=\"Estimated Minor Allele Frequency\">\n");
    ifprintf(vcfdosepartial,"##INFO=<ID=R2,Number=1,Type=Float,Description=\"Estimated Imputation Accuracy (R-square)\">\n");
    ifprintf(vcfdosepartial,"##INFO=<ID=TRAINING,Number=0,Type=Flag,Description=\"Marker was used to train meta-imputation weights\">\n");

    if(myUserVariables.infoDetails)
    {
        ifprintf(vcfdosepartial,"##INFO=<ID=NST,Number=1,Type=Integer,Description=\"Number of studies marker was found during meta-imputation\">\n");
        for(int i=0; i<NoInPrefix; i++)
            ifprintf(vcfdosepartial,"##INFO=<ID=S%d,Number=0,Type=Flag,Description=\"Marker was present in Study %d\">\n",i+1,i+1);
    }
    if(myUserVariables.GT)
        ifprintf(vcfdosepartial,"##FORMAT=<ID=GT,Number=1,Type=String,Description=\"Genotype\">\n");
    if(myUserVariables.DS)
        ifprintf(vcfdosepartial,"##FORMAT=<ID=DS,Number=1,Type=Float,Description=\"Estimated Alternate Allele Dosage : [P(0/1)+2*P(1/1)]\">\n");
    if(myUserVariables.HDS)
        ifprintf(vcfdosepartial,"##FORMAT=<ID=HDS,Number=2,Type=Float,Description=\"Estimated Haploid Alternate Allele Dosage \">\n");
    if(myUserVariables.GP)
        ifprintf(vcfdosepartial,"##FORMAT=<ID=GP,Number=3,Type=Float,Description=\"Estimated Posterior Probabilities for Genotypes 0/0, 0/1 and 1/1 \">\n");
    if(myUserVariables.SD)
        ifprintf(vcfdosepartial,"##FORMAT=<ID=SD,Number=1,Type=Float,Description=\"Variance of Posterior Genotype Probabilities\">\n");

    ifprintf(vcfdosepartial,"##metaMinimac_Command=%s\n", myUserVariables.CommandLine.c_str());

    ifprintf(vcfdosepartial,"#CHROM\tPOS\tID\tREF\tALT\tQUAL\tFILTER\tINFO\tFORMAT");

    for(int Id=0;Id<InputData[0].numSamples;Id++)
    {
        ifprintf(vcfdosepartial,"\t%s",InputData[0].individualName[Id].c_str());
    }
    ifprintf(vcfdosepartial,"\n");

    ifclose(vcfdosepartial);

    if(myUserVariables.debug)
    {
        metaWeight = ifopen(myUserVariables.outfile + ".metaWeights"+(myUserVariables.gzip ? ".gz" : ""), "wb", myUserVariables.gzip ? InputFile::BGZF : InputFile::UNCOMPRESSED);
        WeightPrintStringPointer = (char*)malloc(sizeof(char) * (myUserVariables.PrintBuffer));
        if(metaWeight==NULL)
        {
            cout <<"\n\n ERROR !!! \n Could NOT create the following file : "<< myUserVariables.outfile + ".metaWeights"+(myUserVariables.gzip ? ".gz" : "") <<endl;
            return false;
        }
        ifprintf(metaWeight,"##fileformat=NA\n");
        time_t t = time(0);
        struct tm * now = localtime( & t );
        ifprintf(metaWeight,"##filedate=%d.%d.%d\n",(now->tm_year + 1900),(now->tm_mon + 1) ,now->tm_mday);
        ifprintf(metaWeight,"##source=MetaMinimac2.v%s\n",VERSION);
        ifprintf(metaWeight,"##contig=<ID=%s>\n", finChromosome.c_str());
//        ifprintf(metaWeight,"##INFO=<ID=AF,Number=1,Type=Float,Description=\"Estimated Alternate Allele Frequency\">\n");
//        ifprintf(metaWeight,"##INFO=<ID=MAF,Number=1,Type=Float,Description=\"Estimated Minor Allele Frequency\">\n");
        ifprintf(metaWeight,"##metaMinimac_Command=%s\n",myUserVariables.CommandLine.c_str());

        ifprintf(metaWeight,"#CHROM\tPOS\tID\tREF\tALT");

        for(int Id=0;Id<InputData[0].numSamples;Id++)
        {
            ifprintf(metaWeight,"\t%s",InputData[0].individualName[Id].c_str());
        }
        ifprintf(metaWeight,"\n");
        ifclose(metaWeight);

    }


    return true;
}


string MetaMinimac::GetDosageFileFullName(String prefix)
{
    if(doesExistFile(prefix+".dose.vcf"))
        return (string)prefix+".dose.vcf";
    else if(doesExistFile(prefix+".dose.vcf.gz"))
        return (string)prefix+".dose.vcf.gz";
    return "";
}


bool MetaMinimac::doesExistFile(String filename)
{
    IFILE ifs = ifopen(filename.c_str(), "r");
    if (ifs)
    {
        ifclose(ifs);
        return true;
    }
    else
    {
        ifclose(ifs);
        return false;
    }
}

bool MetaMinimac::LoadEmpVariantInfo()
{
    cout<<"\n Scanning input empirical VCFs for commonly typed SNPs ... "<<endl;
    int time_start = time(0);
    for(int i=0;i<NoInPrefix;i++)
    {
        InputData[i].LoadEmpVariantList();
        cout<<" -- Study "<<i+1<<" #Genotyped Sites = "<<InputData[i].noTypedMarkers<<endl;
    }
    finChromosome = InputData[0].finChromosome;
    FindCommonGenotypedVariants();

    cout<<" -- Found " << NoCommonTypedVariants <<" commonly genotyped! "<<endl;
    cout<<" -- Successful (" << (time(0)-time_start) << " seconds) !!!" << endl;
    return true;

}

void MetaMinimac::FindCommonGenotypedVariants()
{
    std::map<string, int > HashUnionVariantMap;

    for(int i=1;i<NoInPrefix;i++)
    {
        for(int j=0;j<InputData[i].noTypedMarkers;j++)
        {
            variant *thisVariant=&InputData[i].TypedVariantList[j];
            HashUnionVariantMap[thisVariant->name]++;
        }
    }

    TransitionProb.clear();
    int lastbp = 0;
    for(int j=0; j<InputData[0].noTypedMarkers; j++)
    {
        variant *thisVariant = &InputData[0].TypedVariantList[j];
        if(HashUnionVariantMap[thisVariant->name]==NoInPrefix-1)
        {
            CommonGenotypeVariantNameList.push_back(thisVariant->name);
            variant tempVariant;
            tempVariant.chr=thisVariant->chr;
            tempVariant.bp=thisVariant->bp;
            tempVariant.name=thisVariant->name;
            tempVariant.altAlleleString = thisVariant->altAlleleString;
            tempVariant.refAlleleString = thisVariant->refAlleleString;
            CommonTypedVariantList.push_back(tempVariant);
            int distance = tempVariant.bp - lastbp;
            double prob  = max(RECOM_MIN, 1-exp(-lambda*distance));
            TransitionProb.push_back(prob);
            lastbp = tempVariant.bp;
        }
    }

    NoCommonTypedVariants = CommonGenotypeVariantNameList.size();

    for(int i=0;i<NoInPrefix;i++)
    {
        InputData[i].ClearEmpVariantList();
    }
}


void MetaMinimac::FindCurrentMinimumPosition() {

    if (NoInPrefix == 2) {
        int a = CurrentRecordFromStudy[0]->get1BasedPosition();
        int b = CurrentRecordFromStudy[1]->get1BasedPosition();
        CurrentFirstVariantBp = a;
        NoStudiesHasVariant = 1;
        StudiesHasVariant[0] = 0;

        if (b == a) {
            if (IsVariantEqual(*CurrentRecordFromStudy[0], *CurrentRecordFromStudy[1]) == 1) {
                NoStudiesHasVariant = 2;
                StudiesHasVariant[1] = 1;
            }
        } else if (b < a) {
            StudiesHasVariant[0] = 1;
            CurrentFirstVariantBp = b;
        }

    }

    else
    {

        CurrentFirstVariantBp=CurrentRecordFromStudy[0]->get1BasedPosition();

        for(int i=1;i<NoInPrefix;i++)
            if(CurrentRecordFromStudy[i]->get1BasedPosition() < CurrentFirstVariantBp)
                CurrentFirstVariantBp=CurrentRecordFromStudy[i]->get1BasedPosition();

        NoStudiesHasVariant=0;
        VcfRecord *minRecord;
        minRecord = new VcfRecord();
        int Begin=0;
        for(int i=0;i<NoInPrefix;i++)
        {
            if(CurrentRecordFromStudy[i]->get1BasedPosition() == CurrentFirstVariantBp)
            {
                if(Begin==0)
                {
                    Begin=1;
                    minRecord=CurrentRecordFromStudy[i];
                    StudiesHasVariant[NoStudiesHasVariant] = i;
                    NoStudiesHasVariant++;
                }
                else if (IsVariantEqual(*minRecord, *CurrentRecordFromStudy[i])==1)
                {
                    StudiesHasVariant[NoStudiesHasVariant] = i;
                    NoStudiesHasVariant++;
                }
            }
        }
    }
}

int MetaMinimac::IsVariantEqual(VcfRecord &Rec1, VcfRecord &Rec2)
{
    if(strcmp(Rec1.getRefStr(),Rec2.getRefStr())!=0)
        return 0;
    if(strcmp(Rec1.getAltStr(),Rec2.getAltStr())!=0)
        return 0;
    return 1;
}



void MetaMinimac::UpdateCurrentRecords()
{
    for(int i=0; i<NoStudiesHasVariant;i++)
    {
        int index = StudiesHasVariant[i];
        if(!InputDosageStream[index]->readRecord(*CurrentRecordFromStudy[index]))
            CurrentRecordFromStudy[index]->set1BasedPosition(MAXBP);
    }
}


void MetaMinimac::LoadLooDosage()
{
    printf(" -- Loading Empirical Dosage Data ...\n");
    for(int i=0; i<NoInPrefix; i++)
        InputData[i].ReadBasedOnSortCommonGenotypeList(CommonGenotypeVariantNameList, StartSamId, EndSamId);
}

String MetaMinimac::PerformFinalAnalysis()
{
    cout << endl;
    cout << " ------------------------------------------------------------------------------" << endl;
    cout << "                           META-IMPUTATION ANALYSIS                            " << endl;
    cout << " ------------------------------------------------------------------------------" << endl;


    int maxVcfSample = myUserVariables.VcfBuffer;

    StartSamId = 0;

    batchNo = 0;

    int start_time, time_tot;

    while(true)
    {
        batchNo++;
        EndSamId = StartSamId + (maxVcfSample) < NoSamples ? StartSamId + (maxVcfSample) : NoSamples;
        cout << "\n Meta-Imputing Sample " << StartSamId + 1 << "-" << EndSamId << " [" << setprecision(1) << fixed << 100 * (float) EndSamId / NoSamples << "%] ..." << endl;

        start_time = time(0);

        // Read Data From empiricalDose
        LoadLooDosage();

        // Calculate weights
        CalculateWeights();

        // Meta-Imputation
        MetaImputeAndOutput();

        time_tot = time(0) - start_time;
        cout << " -- Successful (" << time_tot << " seconds) !!! " << endl;

        StartSamId = EndSamId;

        if (StartSamId >= NoSamples)
            break;
    }

    if(batchNo > 1)
    {
        AppendtoMainVcf();

        if(myUserVariables.debug)
        {
            AppendtoMainWeightsFile();
        }
    }

    return "Success";
}


void MetaMinimac::CalculateWeights()
{
    cout << " -- Calculating Weights ... " << endl;
    InitiateWeights();
    CalculateLeftProbs();
    CalculatePosterior();
}

void MetaMinimac::InitiateWeights()
{
    int NoSamplesThisBatch = EndSamId-StartSamId;
    Weights.clear();
    Weights.resize(NoCommonTypedVariants);
    for(int i=0; i<NoCommonTypedVariants; i++)
    {
        vector<vector<double> > &ThisWeights = Weights[i];
        ThisWeights.resize(2*NoSamplesThisBatch);
        for(int j=0; j<2*NoSamplesThisBatch; j++)
            ThisWeights[j].resize(NoInPrefix);
    }
}

void MetaMinimac::CalculateLeftProbs()
{
    int NoSamplesThisBatch = EndSamId-StartSamId;
    for (int id=0; id<NoSamplesThisBatch; id++)
    {
        int SampleId = StartSamId + id;
        if (InputData[0].SampleNoHaplotypes[SampleId] == 2)
        {
            InitiateLeftProb(2*id);
            InitiateLeftProb(2*id+1);
        }
        else
            InitiateLeftProb(2*id);
    }

    NoCommonVariantsProcessed = 1;
    while(NoCommonVariantsProcessed < NoCommonTypedVariants)
    {
        Recom = TransitionProb[NoCommonTypedVariants-NoCommonVariantsProcessed];
        NoCommonVariantsProcessed++;
        for (int id=0; id<NoSamplesThisBatch; id++)
        {
            int SampleId = StartSamId + id;
            if (InputData[0].SampleNoHaplotypes[SampleId] == 2)
            {
                UpdateOneStepLeft(2*id);
                UpdateOneStepLeft(2*id+1);
            }
            else
                UpdateOneStepLeft(2*id);
        }
    }

}

void MetaMinimac::InitiateLeftProb(int HapInBatch)
{
    LogOddsModel ThisSampleAnalysis;
    ThisSampleAnalysis.reinitialize(HapInBatch, this);
    vector<double> init(NoInPrefix-1, 0.0);
    vector<double> MiniMizer = Simplex(ThisSampleAnalysis, init);

    vector<double> InitProb;
    InitProb.resize(NoInPrefix);
    logitTransform(MiniMizer, InitProb);

    float ThisGT = InputData[0].TypedGT[HapInBatch][NoCommonTypedVariants-1];
    vector<double> &ThisWeights = Weights[NoCommonTypedVariants-1][HapInBatch];

    for(int i=0; i<NoInPrefix; i++)
    {
        InitProb[i]+=backgroundError;
        float ThisLooDosage = InputData[i].LooDosage[HapInBatch][NoCommonTypedVariants-1];
        InitProb[i] *= (ThisGT==1)?(ThisLooDosage+backgroundError):(1-ThisLooDosage+backgroundError);
        ThisWeights[i] = InitProb[i];
    }
}

void MetaMinimac::UpdateOneStepLeft(int HapInBatch)
{
    double r = Recom*1.0/NoInPrefix, complement = 1-Recom;
    float ThisGT = InputData[0].TypedGT[HapInBatch][NoCommonTypedVariants-NoCommonVariantsProcessed];
    vector<double> &ThisLeftProb = Weights[NoCommonTypedVariants-NoCommonVariantsProcessed][HapInBatch];
    vector<double> &ThisPrevLeftProb = Weights[NoCommonTypedVariants-NoCommonVariantsProcessed+1][HapInBatch];

    double sum = 0.0;
    for(int i=0; i<NoInPrefix; i++)
    {
        for(int j=0; j<NoInPrefix; j++)
            ThisLeftProb[i] += ThisPrevLeftProb[j]*r;
        ThisLeftProb[i] += ThisPrevLeftProb[i]*complement;

        float ThisLooDosage = InputData[i].LooDosage[HapInBatch][NoCommonTypedVariants-NoCommonVariantsProcessed];
        ThisLeftProb[i] *= (ThisGT==1)?(ThisLooDosage+backgroundError):(1-ThisLooDosage+backgroundError);
        sum += ThisLeftProb[i];
    }


    while(sum < JumpThreshold)
    {
        sum = 0.0;
        for(int i=0; i<NoInPrefix; i++)
        {
            ThisLeftProb[i] *= JumpFix;
            sum += ThisLeftProb[i];
        }
    }

}

void MetaMinimac::CalculatePosterior()
{
    int NoSamplesThisBatch = EndSamId-StartSamId;
    PrevRightProb.clear();
    PrevRightProb.resize(2*NoSamplesThisBatch);
    for (int id=0; id<NoSamplesThisBatch; id++)
    {
        int SampleId = StartSamId + id;
        if (InputData[0].SampleNoHaplotypes[SampleId] == 2)
        {
            InitiateRightProb(2*id);
            InitiateRightProb(2*id+1);
        }
        else
            InitiateRightProb(2*id);
    }

    NoCommonVariantsProcessed = 1;
    while(NoCommonVariantsProcessed < NoCommonTypedVariants)
    {
        Recom = TransitionProb[NoCommonVariantsProcessed];
        for (int id=0; id<NoSamplesThisBatch; id++)
        {
            int SampleId = StartSamId + id;
            if (InputData[0].SampleNoHaplotypes[SampleId] == 2)
            {
                UpdateOneStepRight(2*id);
                UpdateOneStepRight(2*id+1);
            }
            else
                UpdateOneStepRight(2*id);
        }
        NoCommonVariantsProcessed++;
    }

}

void MetaMinimac::InitiateRightProb(int HapInBatch)
{
    PrevRightProb[HapInBatch].resize(NoInPrefix, 1.0);
}


void MetaMinimac::UpdateOneStepRight(int HapInBatch)
{

    double r = Recom*1.0/NoInPrefix, complement = 1-Recom;
    float ThisGT = InputData[0].TypedGT[HapInBatch][NoCommonVariantsProcessed-1];
    vector<double> &ThisWeight = Weights[NoCommonVariantsProcessed][HapInBatch];
    vector<double> &ThisPrevRightProb = PrevRightProb[HapInBatch];
    vector<double> ThisRightProb;
    ThisRightProb.resize(NoInPrefix, 0.0);

    for(int i=0; i<NoInPrefix; i++)
    {
        float ThisLooDosage = InputData[i].LooDosage[HapInBatch][NoCommonVariantsProcessed-1];
        ThisPrevRightProb[i] *= (ThisGT==1)?(ThisLooDosage+backgroundError):(1-ThisLooDosage+backgroundError);
    }

    double sum = 0.0;
    for(int i=0; i<NoInPrefix; i++)
    {
        for(int j=0; j<NoInPrefix; j++)
            ThisRightProb[i] += ThisPrevRightProb[j]*r;
        ThisRightProb[i] += ThisPrevRightProb[i]*complement;
        sum += ThisRightProb[i];
    }

    while(sum < JumpThreshold)
    {
        sum = 0.0;
        for(int i=0; i<NoInPrefix; i++)
        {
            ThisRightProb[i] *= JumpFix;
            sum += ThisRightProb[i];
        }
    }

    for(int i=0; i<NoInPrefix; i++)
    {
        ThisWeight[i] *= ThisRightProb[i];
        ThisPrevRightProb[i] = ThisRightProb[i];
    }
}

void MetaMinimac::MetaImputeAndOutput()
{
    printf(" -- Gathering Dosage Data and Saving Results ...\n");

    OpenStreamInputDosageFiles(false);

    if(myUserVariables.VcfBuffer<NoSamples)
    {
        OpenTempOutputFiles();
        OutputPartialVcf();
    }
    else
    {
        OutputAllVcf();
    }

    CloseStreamInputDosageFiles();

}

void MetaMinimac::OutputPartialVcf()
{
    NoVariants = 0;
    NoCommonVariantsProcessed = 0;
    int NoRecordProcessed = 0;

    PrevBp = 0, CurrBp = CommonTypedVariantList[0].bp;
    PrevWeights = &Weights[0], CurrWeights = &Weights[0];

    BufferBp = 0;
    BufferNoVariants = 0;

    if(batchNo==1)
    {
        do
        {
            FindCurrentMinimumPosition();
            if(CurrentFirstVariantBp>BufferBp)
            {
                MetaImputeCurrentBuffer();
                ClearCurrentBuffer();
                if(CurrentFirstVariantBp == MAXBP) break;
                while(CurrentFirstVariantBp == CurrBp)
                {
                    if(myUserVariables.debug) PrintMetaWeight();
                    UpdateWeights();
                }
            }
            ReadCurrentDosageData();
            UpdateCurrentRecords();
            NoRecordProcessed++;
        }while(true);
        NoRecords = NoRecordProcessed;

        if(SnpPrintStringPointerLength > 0)
        {
            ifprintf(vcfsnppartial,"%s",SnpPrintStringPointer);
            SnpPrintStringPointerLength=0;
        }
        ifclose(vcfsnppartial);
    }
    else
    {
        while(NoRecordProcessed<NoRecords)
        {
            FindCurrentMinimumPosition();
            if(CurrentFirstVariantBp>BufferBp)
            {
                MetaImputeCurrentBuffer2();
                ClearCurrentBuffer();
                while(CurrentFirstVariantBp == CurrBp)
                {
                    if(myUserVariables.debug) PrintMetaWeight();
                    UpdateWeights();
                }
            }
            ReadCurrentDosageData();
            UpdateCurrentRecords();
            NoRecordProcessed++;
        }
        if(BufferNoVariants>0)
        {
            MetaImputeCurrentBuffer2();
            ClearCurrentBuffer();
        }
    }

    if (myUserVariables.infoDetails && RsqPrintStringPointerLength > 0)
    {
        ifprintf(vcfrsqpartial,"%s",RsqPrintStringPointer);
        RsqPrintStringPointerLength = 0;
    }
    ifclose(vcfrsqpartial);

    if (VcfPrintStringPointerLength > 0)
    {
        ifprintf(vcfdosepartial,"%s",VcfPrintStringPointer);
        VcfPrintStringPointerLength = 0;
    }
    ifclose(vcfdosepartial);

    if(myUserVariables.debug)
    {
        if(WeightPrintStringPointerLength > 0)
            ifprintf(vcfweightpartial, "%s", WeightPrintStringPointer);
        ifclose(vcfweightpartial);
    }
}

void MetaMinimac::OutputAllVcf()
{
    VcfPrintStringPointerLength=0;
    vcfdosepartial = ifopen(myUserVariables.outfile + ".metaDose.vcf"+ (myUserVariables.gzip ? ".gz" : ""), "a", myUserVariables.gzip ? InputFile::BGZF : InputFile::UNCOMPRESSED);
    if(myUserVariables.debug)
    {
        WeightPrintStringPointerLength=0;
        vcfweightpartial = ifopen(myUserVariables.outfile + ".metaWeights"+ (myUserVariables.gzip ? ".gz" : ""), "a", myUserVariables.gzip ? InputFile::BGZF : InputFile::UNCOMPRESSED);
    }

    NoVariants = 0;
    NoCommonVariantsProcessed = 0;
    int NoRecordProcessed = 0;

    PrevBp = 0, CurrBp = CommonTypedVariantList[0].bp;
    PrevWeights = &Weights[0], CurrWeights = &Weights[0];

    BufferBp = 0;
    BufferNoVariants = 0;

    do
    {
        FindCurrentMinimumPosition();
        if(CurrentFirstVariantBp>BufferBp)
        {
            MetaImputeCurrentBuffer3();
            ClearCurrentBuffer();
            if(CurrentFirstVariantBp == MAXBP) break;
            while(CurrentFirstVariantBp == CurrBp)
            {
                if(myUserVariables.debug)
                {
                    PrintWeightVariantInfo();
                    PrintMetaWeight();
                }
                UpdateWeights();
            }
        }
        ReadCurrentDosageData();
        UpdateCurrentRecords();
        NoRecordProcessed++;
    }while(true);
    NoRecords = NoRecordProcessed;

    if (VcfPrintStringPointerLength > 0)
    {
        ifprintf(vcfdosepartial,"%s",VcfPrintStringPointer);
        VcfPrintStringPointerLength = 0;
    }
    ifclose(vcfdosepartial);

    if(myUserVariables.debug)
    {
        if(WeightPrintStringPointerLength > 0)
            ifprintf(vcfweightpartial, "%s", WeightPrintStringPointer);
        ifclose(vcfweightpartial);
    }
}

void logitTransform(vector<double> &From,vector<double> &To)
{

    double sum=1.0;
    int NoDimensions = (int)To.size();
    for(int i=0; i < (NoDimensions-1); i++) sum+=exp(From[i]);
    for(int i=0; i < (NoDimensions-1); i++)  To[i]=exp(From[i])/sum;
    To[NoDimensions-1]=1.0/sum;


    double checkSum=0.0;
    for(int i=0;i<To.size();i++)
        checkSum+=To[i];
    if(checkSum>1.0001)
        abort();
}

void MetaMinimac::OpenTempOutputFiles()
{
    stringstream ss;
    ss << (batchNo);
    string PartialVcfFileName(myUserVariables.outfile);
    PartialVcfFileName += ".metaDose.part."+(string)(ss.str())+".vcf" + (myUserVariables.gzip ? ".gz" : "");
    vcfdosepartial = ifopen(PartialVcfFileName.c_str(), "wb", myUserVariables.gzip ? InputFile::BGZF : InputFile::UNCOMPRESSED);
    VcfPrintStringPointerLength=0;

    if(myUserVariables.infoDetails)
    {
        string PartialRsqFileName(myUserVariables.outfile);
        PartialRsqFileName += ".metaR2.part."+(string)(ss.str()) + (myUserVariables.gzip ? ".gz" : "");
        vcfrsqpartial = ifopen(PartialRsqFileName.c_str(), "wb", myUserVariables.gzip ? InputFile::BGZF : InputFile::UNCOMPRESSED);
        RsqPrintStringPointerLength=0;
        RsqPrintStringPointer = (char*)malloc(sizeof(char) * (myUserVariables.PrintBuffer));
    }

    if(batchNo==1)
    {
        string PartialVcfFileHeaderName(myUserVariables.outfile);
        stringstream sss;
        sss << 0;
        PartialVcfFileHeaderName += ".metaDose.part."+(string)(sss.str())+".vcf" + (myUserVariables.gzip ? ".gz" : "");
        vcfsnppartial = ifopen(PartialVcfFileHeaderName.c_str(), "wb", myUserVariables.gzip ? InputFile::BGZF : InputFile::UNCOMPRESSED);
        SnpPrintStringPointerLength = 0;
        SnpPrintStringPointer = (char*)malloc(sizeof(char) * (myUserVariables.PrintBuffer));
    }


    if(myUserVariables.debug)
    {
        string PartialWeightFileName(myUserVariables.outfile);
        PartialWeightFileName += ".metaWeights.part."+(string)(ss.str()) + (myUserVariables.gzip ? ".gz" : "");
        vcfweightpartial = ifopen(PartialWeightFileName.c_str(), "wb", myUserVariables.gzip ? InputFile::BGZF : InputFile::UNCOMPRESSED);
        WeightPrintStringPointerLength = 0;
    }

}

void MetaMinimac::ReadCurrentDosageData()
{
    VcfRecord* tempRecord = CurrentRecordFromStudy[StudiesHasVariant[0]];
    variant tempVariant;
    tempVariant.chr  = tempRecord->getChromStr();
    tempVariant.bp   = tempRecord->get1BasedPosition();
    tempVariant.refAlleleString = tempRecord->getRefStr();
    tempVariant.altAlleleString = tempRecord->getAltStr();
    tempVariant.name = tempVariant.chr+":"+to_string(tempVariant.bp)+":"+ tempVariant.refAlleleString+":"+tempVariant.altAlleleString;

    int VariantId = 0;
    for(vector<variant>::iterator thisVariant = BufferVariantList.begin(); thisVariant < BufferVariantList.end(); ++thisVariant)
    {
        if(tempVariant.name == thisVariant->name)
        {
            int count = thisVariant->NoStudiesHasVariant;
            for(int i=0; i<NoStudiesHasVariant; i++)
            {
                thisVariant->StudiesHasVariant[count]=StudiesHasVariant[i];
                count++;
            }
            thisVariant->NoStudiesHasVariant = count;
            break;
        }
        VariantId++;
    }
    if(VariantId==BufferNoVariants)
    {
        tempVariant.NoStudiesHasVariant = NoStudiesHasVariant;
        tempVariant.StudiesHasVariant = StudiesHasVariant;
        BufferVariantList.push_back(tempVariant);
        BufferNoVariants++;
    }

    for(int j=0; j<NoStudiesHasVariant; j++)
    {
        int index = StudiesHasVariant[j];
        InputData[index].LoadData(VariantId,CurrentRecordFromStudy[index]->getGenotypeInfo(), StartSamId, EndSamId);
    }
}

void MetaMinimac::MetaImputeCurrentBuffer()
{
    for(int VariantId=0; VariantId<BufferNoVariants; VariantId++)
    {
        CurrentVariant = &BufferVariantList[VariantId];
        CreateMetaImputedData(VariantId);
        PrintVariantPartialInfo();
        PrintMetaImputedData();
        PrintMetaImputedRsq();
    }
}

void MetaMinimac::MetaImputeCurrentBuffer2()
{
    for(int VariantId=0; VariantId<BufferNoVariants; VariantId++)
    {
        CurrentVariant = &BufferVariantList[VariantId];
        CreateMetaImputedData(VariantId);
        PrintMetaImputedData();
        PrintMetaImputedRsq();
    }
}

void MetaMinimac::MetaImputeCurrentBuffer3()
{
    for(int VariantId=0; VariantId<BufferNoVariants; VariantId++)
    {
        CurrentVariant = &BufferVariantList[VariantId];
        CreateMetaImputedData(VariantId);
        PrintVariantInfo();
        PrintMetaImputedData();
    }
}


void MetaMinimac::CreateMetaImputedData(int VariantId)
{
    CurrentHapDosageSum = 0;
    CurrentHapDosageSumSq = 0;
    CurrentMetaImputedDosage.clear();
    if(CurrentVariant->NoStudiesHasVariant==1)
    {
        CurrentMetaImputedDosage=InputData[CurrentVariant->StudiesHasVariant[0]].BufferHapDosage[0];
        for(int i=0; i<2*(EndSamId-StartSamId); i++)
        {
            CurrentHapDosageSum += CurrentMetaImputedDosage[i];
            CurrentHapDosageSumSq += CurrentMetaImputedDosage[i]*CurrentMetaImputedDosage[i];
        }
    }
    else
    {
        CurrentMetaImputedDosage.resize(2*(EndSamId-StartSamId));
        for (int j=0; j<CurrentVariant->NoStudiesHasVariant; j++)
        {
            int index = CurrentVariant->StudiesHasVariant[j];
            InputData[index].GetData(VariantId);
        }
        for(int id=0; id<EndSamId-StartSamId; id++)
        {
            int SampleId = StartSamId+id;
            if(InputData[0].SampleNoHaplotypes[SampleId]==2)
            {
                MetaImpute(2*id);
                MetaImpute(2*id + 1);
            }
            else
            {
                MetaImpute(2*id);
            }
        }
    }
}

void MetaMinimac::UpdateWeights()
{
    NoCommonVariantsProcessed++;
    PrevWeights = CurrWeights;
    PrevBp      = CurrBp;
    if(NoCommonVariantsProcessed < NoCommonTypedVariants)
    {
        CurrWeights   = &Weights[NoCommonVariantsProcessed];
        CurrBp        = CommonTypedVariantList[NoCommonVariantsProcessed].bp;
    }
    else
    {
        CurrBp        = MAXBP;
    }
}

void MetaMinimac::MetaImpute(int Sample)
{
    vector<double>& ThisPrevWeights = (*PrevWeights)[Sample];
    vector<double>& ThisCurrWeights = (*CurrWeights)[Sample];

    double WeightSum = 0.0;
    double Dosage = 0.0;

    for (int j=0; j<CurrentVariant->NoStudiesHasVariant; j++)
    {
        int index = CurrentVariant->StudiesHasVariant[j];
        double Weight = (ThisPrevWeights[index]*(CurrBp-BufferBp)+ThisCurrWeights[index]*(BufferBp-PrevBp))*1.0/(CurrBp-PrevBp);
        WeightSum += Weight;
        Dosage += Weight * InputData[index].CurrentHapDosage[Sample];
    }
    Dosage /= WeightSum;

    CurrentMetaImputedDosage[Sample] = Dosage;
    CurrentHapDosageSum += Dosage;
    CurrentHapDosageSumSq += Dosage * Dosage;
}


void MetaMinimac::PrintMetaImputedData()
{
    for(int id=0; id<EndSamId-StartSamId; id++)
    {
        if( InputData[0].SampleNoHaplotypes[StartSamId+id]==2 )
            PrintDiploidDosage((CurrentMetaImputedDosage[2*id]), (CurrentMetaImputedDosage[2*id+1]));
        else
            PrintHaploidDosage((CurrentMetaImputedDosage[2*id]));
    }

    VcfPrintStringPointerLength+=sprintf(VcfPrintStringPointer+VcfPrintStringPointerLength,"\n");
    if(VcfPrintStringPointerLength > 0.9 * (float)(myUserVariables.PrintBuffer))
    {
        ifprintf(vcfdosepartial,"%s",VcfPrintStringPointer);
        VcfPrintStringPointerLength=0;
    }

}

void MetaMinimac::PrintMetaImputedRsq()
{
    if(myUserVariables.infoDetails)
    {
        RsqPrintStringPointerLength+=sprintf(RsqPrintStringPointer+RsqPrintStringPointerLength,"%.8f\t%.8f\n", CurrentHapDosageSum, CurrentHapDosageSumSq);
        if(RsqPrintStringPointerLength > 0.9 * (float)(myUserVariables.PrintBuffer))
        {
            ifprintf(vcfrsqpartial,"%s",RsqPrintStringPointer);
            RsqPrintStringPointerLength=0;
        }
    }
}

void MetaMinimac::PrintMetaWeight()
{
    for(int id=0; id<EndSamId-StartSamId; id++)
    {
        WeightPrintStringPointerLength += sprintf(WeightPrintStringPointer+WeightPrintStringPointerLength,"\t");
        if(InputData[0].SampleNoHaplotypes[StartSamId+id]==2)
        {
            PrintWeightForHaplotype(2*id);
            WeightPrintStringPointerLength += sprintf(WeightPrintStringPointer+WeightPrintStringPointerLength,"|");
            PrintWeightForHaplotype(2*id+1);
        }
        else
            PrintWeightForHaplotype(2*id);
    }

    WeightPrintStringPointerLength+= sprintf(WeightPrintStringPointer+WeightPrintStringPointerLength,"\n");
    if(WeightPrintStringPointerLength > 0.9 * (float)(myUserVariables.PrintBuffer))
    {
        ifprintf(vcfweightpartial, "%s", WeightPrintStringPointer);
        WeightPrintStringPointerLength = 0;
    }
}


void MetaMinimac::PrintDiploidDosage(float &x, float &y)
{

    bool colonIndex=false;
    VcfPrintStringPointerLength+=sprintf(VcfPrintStringPointer+VcfPrintStringPointerLength,"\t");

    if(x<0.0005 && y<0.0005)
    {
        if(myUserVariables.GT)
        {

            VcfPrintStringPointerLength+=sprintf(VcfPrintStringPointer+VcfPrintStringPointerLength,"0|0");
            colonIndex=true;
        }
        if(myUserVariables.DS)
        {

            if(colonIndex)
                VcfPrintStringPointerLength+=sprintf(VcfPrintStringPointer+VcfPrintStringPointerLength,":");
            VcfPrintStringPointerLength+=sprintf(VcfPrintStringPointer+VcfPrintStringPointerLength,"0");
            colonIndex=true;
        }
        if(myUserVariables.HDS)
        {

            if(colonIndex)
                VcfPrintStringPointerLength+=sprintf(VcfPrintStringPointer+VcfPrintStringPointerLength,":");
            VcfPrintStringPointerLength+=sprintf(VcfPrintStringPointer+VcfPrintStringPointerLength,"0,0");
            colonIndex=true;
        }
        if(myUserVariables.GP)
        {

            if(colonIndex)
                VcfPrintStringPointerLength+=sprintf(VcfPrintStringPointer+VcfPrintStringPointerLength,":");
            colonIndex=true;
            VcfPrintStringPointerLength+=sprintf(VcfPrintStringPointer+VcfPrintStringPointerLength,"1,0,0");
        }
        if(myUserVariables.SD)
        {
            if(colonIndex)
                VcfPrintStringPointerLength+=sprintf(VcfPrintStringPointer+VcfPrintStringPointerLength,":");
            colonIndex=true;
            VcfPrintStringPointerLength+=sprintf(VcfPrintStringPointer+VcfPrintStringPointerLength,"0");
        }
        return;
    }


    if(myUserVariables.GT)
    {

        VcfPrintStringPointerLength+=sprintf(VcfPrintStringPointer+VcfPrintStringPointerLength,"%d|%d",(x>0.5),(y>0.5));
        colonIndex=true;
    }
    if(myUserVariables.DS)
    {

        if(colonIndex)
            VcfPrintStringPointerLength+=sprintf(VcfPrintStringPointer+VcfPrintStringPointerLength,":");
        VcfPrintStringPointerLength+=sprintf(VcfPrintStringPointer+VcfPrintStringPointerLength,"%.3f",x+ y);
        colonIndex=true;
    }
    if(myUserVariables.HDS)
    {

        if(colonIndex)
            VcfPrintStringPointerLength+=sprintf(VcfPrintStringPointer+VcfPrintStringPointerLength,":");
        VcfPrintStringPointerLength+=sprintf(VcfPrintStringPointer+VcfPrintStringPointerLength,"%.3f,%.3f",x , y);
        colonIndex=true;
    }
    if(myUserVariables.GP)
    {

        if(colonIndex)
            VcfPrintStringPointerLength+=sprintf(VcfPrintStringPointer+VcfPrintStringPointerLength,":");
        colonIndex=true;
        VcfPrintStringPointerLength+=sprintf(VcfPrintStringPointer+VcfPrintStringPointerLength,"%.3f,%.3f,%.3f",(1-x)*(1-y),x*(1-y)+y*(1-x),x*y);
    }
    if(myUserVariables.SD)
    {
        if(colonIndex)
            VcfPrintStringPointerLength+=sprintf(VcfPrintStringPointer+VcfPrintStringPointerLength,":");
        colonIndex=true;
        VcfPrintStringPointerLength+=sprintf(VcfPrintStringPointer+VcfPrintStringPointerLength,"%.3f", x*(1-x) + y*(1-y));
    }



}

void MetaMinimac::PrintHaploidDosage(float &x)
{
    bool colonIndex=false;
    VcfPrintStringPointerLength+=sprintf(VcfPrintStringPointer+VcfPrintStringPointerLength,"\t");

    if(x<0.0005)
    {
        if(myUserVariables.GT)
        {
            VcfPrintStringPointerLength+=sprintf(VcfPrintStringPointer+VcfPrintStringPointerLength,"0");
            colonIndex=true;
        }
        if(myUserVariables.DS)
        {

            if(colonIndex)
                VcfPrintStringPointerLength+=sprintf(VcfPrintStringPointer+VcfPrintStringPointerLength,":");
            VcfPrintStringPointerLength+=sprintf(VcfPrintStringPointer+VcfPrintStringPointerLength,"0");
            colonIndex=true;
        }
        if(myUserVariables.HDS)
        {

            if(colonIndex)
                VcfPrintStringPointerLength+=sprintf(VcfPrintStringPointer+VcfPrintStringPointerLength,":");
            VcfPrintStringPointerLength+=sprintf(VcfPrintStringPointer+VcfPrintStringPointerLength,"0");
            colonIndex=true;
        }
        if(myUserVariables.GP)
        {

            if(colonIndex)
                VcfPrintStringPointerLength+=sprintf(VcfPrintStringPointer+VcfPrintStringPointerLength,":");
            colonIndex=true;
            VcfPrintStringPointerLength+=sprintf(VcfPrintStringPointer+VcfPrintStringPointerLength,"1,0");
        }
        if(myUserVariables.SD)
        {
            if(colonIndex)
                VcfPrintStringPointerLength+=sprintf(VcfPrintStringPointer+VcfPrintStringPointerLength,":");
            colonIndex=true;
            VcfPrintStringPointerLength+=sprintf(VcfPrintStringPointer+VcfPrintStringPointerLength,"0");
        }
        return;

    }



    if(myUserVariables.GT)
    {
        VcfPrintStringPointerLength+=sprintf(VcfPrintStringPointer+VcfPrintStringPointerLength,"%d",(x>0.5));
        colonIndex=true;
    }
    if(myUserVariables.DS)
    {

        if(colonIndex)
            VcfPrintStringPointerLength+=sprintf(VcfPrintStringPointer+VcfPrintStringPointerLength,":");
        VcfPrintStringPointerLength+=sprintf(VcfPrintStringPointer+VcfPrintStringPointerLength,"%.3f",x);
        colonIndex=true;
    }
    if(myUserVariables.HDS)
    {

        if(colonIndex)
            VcfPrintStringPointerLength+=sprintf(VcfPrintStringPointer+VcfPrintStringPointerLength,":");
        VcfPrintStringPointerLength+=sprintf(VcfPrintStringPointer+VcfPrintStringPointerLength,"%.3f",x );
        colonIndex=true;
    }
    if(myUserVariables.GP)
    {

        if(colonIndex)
            VcfPrintStringPointerLength+=sprintf(VcfPrintStringPointer+VcfPrintStringPointerLength,":");
        colonIndex=true;
        VcfPrintStringPointerLength+=sprintf(VcfPrintStringPointer+VcfPrintStringPointerLength,"%.3f,%.3f",1-x,x);
    }
    if(myUserVariables.SD)
    {
        if(colonIndex)
            VcfPrintStringPointerLength+=sprintf(VcfPrintStringPointer+VcfPrintStringPointerLength,":");
        colonIndex=true;
        VcfPrintStringPointerLength+=sprintf(VcfPrintStringPointer+VcfPrintStringPointerLength,"%.3f", x*(1-x));
    }
}


void MetaMinimac::PrintWeightForHaplotype(int haploId)
{
    vector<double>& ThisCurrWeights = (*CurrWeights)[haploId];
    double WeightSum = 0.0;
    for(int i=0; i<NoInPrefix; i++)
        WeightSum += ThisCurrWeights[i];
    WeightPrintStringPointerLength += sprintf(WeightPrintStringPointer+WeightPrintStringPointerLength,"%0.4f", ThisCurrWeights[0]/WeightSum);
    for(int i=1;i<NoInPrefix;i++)
        WeightPrintStringPointerLength += sprintf(WeightPrintStringPointer+WeightPrintStringPointerLength,",%0.4f", ThisCurrWeights[i]/WeightSum);
}



void MetaMinimac::AppendtoMainVcf()
{
    cout << endl;
    cout << "\n Appending to final output VCF File : " << myUserVariables.outfile + ".metaDose.vcf"+(myUserVariables.gzip ? ".gz" : "") <<endl;

    int start_time = time(0);
    VcfPrintStringPointerLength=0;
    vcfdosepartial = ifopen(myUserVariables.outfile + ".metaDose.vcf" + (myUserVariables.gzip ? ".gz" : ""), "a", myUserVariables.gzip ? InputFile::BGZF : InputFile::UNCOMPRESSED);
    vector<IFILE> vcfdosepartialList(batchNo+1);

    for(int i=0;i<=batchNo;i++)
    {
        stringstream ss;
        ss << (i);
        string PartialVcfFileName(myUserVariables.outfile);
        PartialVcfFileName += ".metaDose.part."+(string)(ss.str())+".vcf"+ (myUserVariables.gzip ? ".gz" : "");
        vcfdosepartialList[i] = ifopen(PartialVcfFileName.c_str(), "r");
    }

    if(myUserVariables.infoDetails)
    {
        vcfrsqpartialList.resize(batchNo);
        for(int i=1;i<=batchNo;i++)
        {
            stringstream ss;
            ss << (i);
            string PartialRsqFileName(myUserVariables.outfile);
            PartialRsqFileName += ".metaR2.part."+(string)(ss.str())+ (myUserVariables.gzip ? ".gz" : "");
            vcfrsqpartialList[i-1] = ifopen(PartialRsqFileName.c_str(), "r");
        }
    }

    string line;

    for(int i=0; i<NoVariants; i++)
    {
        line.clear();
        vcfdosepartialList[0]->readLine(line);
        VcfPrintStringPointerLength+=sprintf(VcfPrintStringPointer+VcfPrintStringPointerLength, "%s%s\t%s",line.c_str(), CreateRsqInfo().c_str(), myUserVariables.formatStringForVCF.c_str());

        for(int j=1;j<=batchNo;j++)
        {
            line.clear();
            vcfdosepartialList[j]->readLine(line);
            VcfPrintStringPointerLength+=sprintf(VcfPrintStringPointer + VcfPrintStringPointerLength,"%s",line.c_str());

        }
        VcfPrintStringPointerLength+=sprintf(VcfPrintStringPointer + VcfPrintStringPointerLength,"\n");
        if(VcfPrintStringPointerLength > 0.9 * (float)(myUserVariables.PrintBuffer))
        {
            ifprintf(vcfdosepartial,"%s",VcfPrintStringPointer);
            VcfPrintStringPointerLength=0;
        }
    }
    if(VcfPrintStringPointerLength > 0)
    {
        ifprintf(vcfdosepartial,"%s",VcfPrintStringPointer);
        VcfPrintStringPointerLength=0;
    }

    for(int i=0;i<=batchNo;i++)
    {
        ifclose(vcfdosepartialList[i]);
        stringstream ss;
        ss << (i);
        string tempFileIndex(myUserVariables.outfile);
        tempFileIndex += ".metaDose.part."+(string)(ss.str())+".vcf"+ (myUserVariables.gzip ? ".gz" : "");
        remove(tempFileIndex.c_str());
    }
    if(myUserVariables.infoDetails)
    {
        vcfrsqpartialList.resize(batchNo);
        for(int i=1;i<=batchNo;i++)
        {
            ifclose(vcfrsqpartialList[i-1]);
            stringstream ss;
            ss << (i);
            string PartialRsqFileName(myUserVariables.outfile);
            PartialRsqFileName += ".metaR2.part."+(string)(ss.str())+ (myUserVariables.gzip ? ".gz" : "");
            remove(PartialRsqFileName.c_str());
        }
    }
    ifclose(vcfdosepartial);

    int time_tot = time(0) - start_time;
    cout << " -- Successful (" << time_tot << " seconds) !!!" << endl;
}

void MetaMinimac::PrintVariantInfo()
{
    VcfPrintStringPointerLength+=sprintf(VcfPrintStringPointer+VcfPrintStringPointerLength, "%s\t%d\t%s\t%s\t%s\t.\tPASS\t%s\t%s",
                                         CurrentVariant->chr.c_str(), CurrentVariant->bp, CurrentVariant->name.c_str(),
                                         CurrentVariant->refAlleleString.c_str(), CurrentVariant->altAlleleString.c_str(),
                                         CreateInfo().c_str(), myUserVariables.formatStringForVCF.c_str());
}

void MetaMinimac::PrintVariantPartialInfo()
{
    SnpPrintStringPointerLength+=sprintf(SnpPrintStringPointer+SnpPrintStringPointerLength, "%s\t%d\t%s\t%s\t%s\t.\tPASS\t%s\n",
                                         CurrentVariant->chr.c_str(), CurrentVariant->bp, CurrentVariant->name.c_str(),
                                         CurrentVariant->refAlleleString.c_str(), CurrentVariant->altAlleleString.c_str(),
                                         CreatePartialInfo().c_str());
    if(SnpPrintStringPointerLength > 0.9 * (float)(myUserVariables.PrintBuffer))
    {
        ifprintf(vcfsnppartial,"%s",SnpPrintStringPointer);
        SnpPrintStringPointerLength=0;
    }
}

string MetaMinimac::CreateInfo()
{
    if(!myUserVariables.infoDetails)
        return ".";

    double hapSum = CurrentHapDosageSum, hapSumSq = CurrentHapDosageSumSq;
    double freq = hapSum*1.0/NoHaplotypes;
    double maf = (freq > 0.5) ? (1.0 - freq) : freq;
    double rsq = 0.0, evar = freq*(1-freq), ovar = 0.0;
    if (NoHaplotypes > 2 & (hapSumSq - hapSum * hapSum / NoHaplotypes) >0 )
    {
        ovar = (hapSumSq - hapSum * hapSum / NoHaplotypes)/ NoHaplotypes;
        rsq = ovar / (evar + 1e-30);
    }

    stringstream ss;

    ss<<"NST=";
    ss<<CurrentVariant->NoStudiesHasVariant;
    for(int i=0; i<CurrentVariant->NoStudiesHasVariant; i++)
    {
        ss<<";S";
        ss<<CurrentVariant->StudiesHasVariant[i]+1;
    }

    if(CurrentVariant->name == CommonGenotypeVariantNameList[NoCommonVariantsProcessed-1])
    {
        ss<<";TRAINING";
    }

    ss<< ";AF=" << fixed << setprecision(5) << freq <<";MAF=";
    ss<< fixed << setprecision(5) << maf <<";R2=";
    ss<< fixed << setprecision(5) << rsq ;

    return ss.str();
}

string MetaMinimac::CreatePartialInfo()
{
    if(!myUserVariables.infoDetails)
        return ".";

    stringstream ss;

    ss<<"NST=";
    ss<<CurrentVariant->NoStudiesHasVariant;
    for(int i=0; i<CurrentVariant->NoStudiesHasVariant; i++)
    {
        ss<<";S";
        ss<<CurrentVariant->StudiesHasVariant[i]+1;
    }

    if(CurrentVariant->name == CommonGenotypeVariantNameList[NoCommonVariantsProcessed-1])
    {
        ss<<";TRAINING";
    }

    return ss.str();
}

string MetaMinimac::CreateRsqInfo()
{
    if(!myUserVariables.infoDetails)
        return "";

    double hapSum = 0.0, hapSumSq = 0.0;
    string line;
    char* sum, * sumsq;
    for(int i=0;i<batchNo;i++)
    {
        line.clear();
        vcfrsqpartialList[i]->readLine(line);
        sum = strtok((char*)line.c_str(), "\t");
        sumsq = strtok(NULL, "\t");

        hapSum += stod(sum);
        hapSumSq += stod(sumsq);
    }

    double freq = hapSum*1.0/NoHaplotypes;
    double maf = (freq > 0.5) ? (1.0 - freq) : freq;
    double rsq = 0.0, evar = freq*(1-freq), ovar = 0.0;
    if (NoHaplotypes > 2 & (hapSumSq - hapSum * hapSum / NoHaplotypes) >0 )
    {
        ovar = (hapSumSq - hapSum * hapSum / NoHaplotypes)/ NoHaplotypes;
        rsq = ovar / (evar + 1e-30);
    }
    stringstream ss;
    ss<< ";AF=" << fixed << setprecision(5) << freq <<";MAF=";
    ss<< fixed << setprecision(5) << maf <<";R2=";
    ss<< fixed << setprecision(5) << rsq ;
    return ss.str();
}

void MetaMinimac::PrintWeightVariantInfo()
{
    variant& tempVariant = CommonTypedVariantList[NoCommonVariantsProcessed];
    WeightPrintStringPointerLength+=sprintf(WeightPrintStringPointer+WeightPrintStringPointerLength,"%s\t%d\t%s\t%s\t%s",
                                            tempVariant.chr.c_str(), tempVariant.bp, tempVariant.name.c_str(),
                                            tempVariant.refAlleleString.c_str(), tempVariant.altAlleleString.c_str());
}

void MetaMinimac::AppendtoMainWeightsFile()
{
    cout << "\n Appending to final output weight file : " << myUserVariables.outfile + ".metaWeights" + (myUserVariables.gzip ? ".gz" : "") <<endl;
    int start_time = time(0);
    WeightPrintStringPointerLength=0;
    metaWeight = ifopen(myUserVariables.outfile + ".metaWeights" + (myUserVariables.gzip ? ".gz" : ""), "a", myUserVariables.gzip ? InputFile::BGZF : InputFile::UNCOMPRESSED);
    vector<IFILE> weightpartialList(batchNo);

    for(int i=1; i<=batchNo; i++)
    {
        stringstream ss;
        ss << (i);
        string PartialWeightFileName(myUserVariables.outfile);
        PartialWeightFileName += ".metaWeights.part."+(string)(ss.str())+(myUserVariables.gzip ? ".gz" : "");
        weightpartialList[i-1] = ifopen(PartialWeightFileName.c_str(), "r");
    }

    string line;

    NoCommonVariantsProcessed = 0;
    for(int i=0; i<NoCommonTypedVariants; i++)
    {
        PrintWeightVariantInfo();
        for(int j=1;j<=batchNo;j++)
        {
            line.clear();
            weightpartialList[j-1]->readLine(line);
            WeightPrintStringPointerLength+=sprintf(WeightPrintStringPointer + WeightPrintStringPointerLength,"%s",line.c_str());
        }
        WeightPrintStringPointerLength+=sprintf(WeightPrintStringPointer + WeightPrintStringPointerLength,"\n");
        if(WeightPrintStringPointerLength > 0.9 * (float)(myUserVariables.PrintBuffer))
        {
            ifprintf(metaWeight, "%s", WeightPrintStringPointer);
            WeightPrintStringPointerLength = 0;
        }
        NoCommonVariantsProcessed++;
    }
    if(WeightPrintStringPointerLength > 0)
    {
        ifprintf(metaWeight, "%s", WeightPrintStringPointer);
        WeightPrintStringPointerLength = 0;
    }

    for(int i=1;i<=batchNo;i++)
    {
        ifclose(weightpartialList[i-1]);
        stringstream ss;
        ss << (i);
        string tempFileIndex(myUserVariables.outfile);
        tempFileIndex += ".metaWeights.part."+(string)(ss.str())+(myUserVariables.gzip ? ".gz" : "");
        remove(tempFileIndex.c_str());
    }
    ifclose(metaWeight);
    int tot_time = time(0) - start_time;
    cout << " -- Successful (" << tot_time << " seconds) !!!" << endl;
}

void MetaMinimac::ClearCurrentBuffer()
{
    NoVariants += BufferNoVariants;
    for(int i=0; i<NoInPrefix; i++)
        InputData[i].ClearBuffer();
    BufferVariantList.clear();
    BufferNoVariants = 0;
    BufferBp = CurrentFirstVariantBp;
}