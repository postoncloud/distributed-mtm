#pragma once

#include "models/encdec.h"
//#include "models/multi_s2s.h"
#include "models/s2s.h"

namespace marian {

class DecoderStateHardAtt : public DecoderState {
protected:
  std::vector<size_t> attentionIndices_;

public:
  DecoderStateHardAtt(const rnn::States& states,
                      Expr probs,
                      Ptr<EncoderState> encState,
                      const std::vector<size_t>& attentionIndices)
      : DecoderState(states, probs, encState),
        attentionIndices_(attentionIndices) {}

  virtual Ptr<DecoderState> select(const std::vector<size_t>& selIdx) {
    std::vector<size_t> selectedAttentionIndices;
    for(auto i : selIdx)
      selectedAttentionIndices.push_back(attentionIndices_[i]);

    return New<DecoderStateHardAtt>(
        states_.select(selIdx), probs_, encState_, selectedAttentionIndices);
  }

  virtual void setAttentionIndices(
      const std::vector<size_t>& attentionIndices) {
    attentionIndices_ = attentionIndices;
  }

  virtual std::vector<size_t>& getAttentionIndices() {
    UTIL_THROW_IF2(attentionIndices_.empty(), "Empty attention indices");
    return attentionIndices_;
  }

  virtual void blacklist(Expr totalCosts, Ptr<data::CorpusBatch> batch) {
    auto attentionIdx = getAttentionIndices();
    int dimVoc = totalCosts->shape()[1];
    for(int i = 0; i < attentionIdx.size(); i++) {
      if(batch->front()->indices()[attentionIdx[i]] != 0) {
        totalCosts->val()->set(i * dimVoc + EOS_ID,
                               std::numeric_limits<float>::lowest());
      } else {
        totalCosts->val()->set(i * dimVoc + STP_ID,
                               std::numeric_limits<float>::lowest());
      }
    }
  }
};

class DecoderHardAtt : public DecoderBase {
protected:
  Ptr<rnn::RNN> rnn_;
  std::unordered_set<Word> specialSymbols_;

public:
  template <class... Args>
  DecoderHardAtt(Ptr<Config> options, Args... args)
      : DecoderBase(options, args...) {
    if(options->has("special-vocab")) {
      auto spec = options->get<std::vector<size_t>>("special-vocab");
      specialSymbols_.insert(spec.begin(), spec.end());
    }
  }

  virtual Ptr<DecoderState> startState(Ptr<EncoderState> encState) {
    using namespace keywords;

    auto meanContext = weighted_average(
        encState->getContext(), encState->getMask(), axis = 2);

    bool layerNorm = options_->get<bool>("layer-normalization");
    auto graph = meanContext->graph();
    auto mlp = mlp::mlp(graph)
               .push_back(mlp::dense(graph)
                          ("prefix", prefix_ + "_ff_state")
                          ("dim", opt<int>("dim-rnn"))
                          ("activation", mlp::act::tanh)
                          ("layer-normalization", opt<bool>("layer-normalization")));
    auto start = mlp->apply(meanContext);

    rnn::States startStates(opt<size_t>("dec-depth"), {start, start});

    return New<DecoderStateHardAtt>(
        startStates, nullptr, encState, std::vector<size_t>({0}));
  }

  virtual Ptr<DecoderState> step(Ptr<ExpressionGraph> graph,
                                 Ptr<DecoderState> state) {
    using namespace keywords;

    int dimTrgVoc = options_->get<std::vector<int>>("dim-vocabs").back();

    int dimTrgEmb = options_->get<int>("dim-emb");

    int dimDecState = options_->get<int>("dim-rnn");
    bool layerNorm = options_->get<bool>("layer-normalization");
    bool skipDepth = options_->get<bool>("skip");
    size_t decoderLayers = options_->get<size_t>("dec-depth");

    float dropoutRnn = inference_ ? 0 : options_->get<float>("dropout-rnn");
    float dropoutTrg = inference_ ? 0 : options_->get<float>("dropout-trg");

    auto cellType = options_->get<std::string>("cell-dec");

    auto stateHardAtt = std::dynamic_pointer_cast<DecoderStateHardAtt>(state);

    auto trgEmbeddings = stateHardAtt->getTargetEmbeddings();

    auto context = stateHardAtt->getEncoderState()->getContext();
    int dimContext = context->shape()[1];
    int dimSrcWords = context->shape()[2];

    int dimBatch = context->shape()[0];
    int dimTrgWords = trgEmbeddings->shape()[2];
    int dimBeam = trgEmbeddings->shape()[3];

    if(dropoutTrg) {
      auto trgWordDrop = graph->dropout(dropoutTrg, {dimBatch, 1, dimTrgWords});
      trgEmbeddings = dropout(trgEmbeddings, mask = trgWordDrop);
    }

    auto flatContext = reshape(context, {dimBatch * dimSrcWords, dimContext});
    auto attendedContext
        = rows(flatContext, stateHardAtt->getAttentionIndices());
    attendedContext = reshape(attendedContext,
                              {dimBatch, dimContext, dimTrgWords, dimBeam});

    auto rnnInputs = concatenate({trgEmbeddings, attendedContext}, axis = 1);
    int dimInput = rnnInputs->shape()[1];

    if(!rnn_) {

      auto rnn = rnn::rnn(graph)
                 ("type", cellType)
                 ("dimInput", dimTrgEmb)
                 ("dimState", dimDecState)
                 ("dropout", dropoutRnn)
                 ("layer-normalization", layerNorm)
                 ("skip", skipDepth)
                 .push_back(rnn::cell(graph)
                            ("prefix", prefix_));

      for(int i = 0; i < decoderLayers - 1; ++i)
        rnn.push_back(rnn::cell(graph)
                      ("prefix", prefix_ + "_l" + std::to_string(i)));

      rnn_ = rnn.construct();

    }

    auto decContext = rnn_->transduce(rnnInputs, stateHardAtt->getStates());
    rnn::States decStates = rnn_->lastCellStates();

    //// 2-layer feedforward network for outputs and cost
    auto out = mlp::mlp(graph)
               .push_back(mlp::dense(graph)
                          ("prefix", prefix_ + "_ff_logit_l1")
                          ("dim", dimTrgEmb)
                          ("activation", mlp::act::tanh)
                          ("layer-normalization", layerNorm))
               .push_back(mlp::dense(graph)
                          ("prefix", prefix_ + "_ff_logit_l2")
                          ("dim", dimTrgVoc));

    auto logits = out->apply(rnnInputs, decContext);

    return New<DecoderStateHardAtt>(decStates,
                                    logits,
                                    stateHardAtt->getEncoderState(),
                                    stateHardAtt->getAttentionIndices());
  }

  virtual std::tuple<Expr, Expr> groundTruth(Ptr<DecoderState> state,
                                             Ptr<ExpressionGraph> graph,
                                             Ptr<data::CorpusBatch> batch,
                                             size_t index) {
    using namespace keywords;

    auto ret = DecoderBase::groundTruth(state, graph, batch, index);

    auto subBatch = (*batch)[index];
    int dimBatch = subBatch->batchSize();
    int dimWords = subBatch->batchWidth();

    std::vector<size_t> attentionIndices(dimBatch, 0);
    std::vector<size_t> currentPos(dimBatch, 0);
    std::iota(currentPos.begin(), currentPos.end(), 0);

    for(int i = 0; i < dimWords - 1; ++i) {
      for(int j = 0; j < dimBatch; ++j) {
        size_t word = subBatch->indices()[i * dimBatch + j];
        if(specialSymbols_.count(word))
          currentPos[j] += dimBatch;
        attentionIndices.push_back(currentPos[j]);
      }
    }

    std::dynamic_pointer_cast<DecoderStateHardAtt>(state)->setAttentionIndices(
        attentionIndices);

    return ret;
  }

  virtual void selectEmbeddings(Ptr<ExpressionGraph> graph,
                                Ptr<DecoderState> state,
                                const std::vector<size_t>& embIdx) {
    DecoderBase::selectEmbeddings(graph, state, embIdx);

    auto stateHardAtt = std::dynamic_pointer_cast<DecoderStateHardAtt>(state);

    int dimSrcWords = state->getEncoderState()->getContext()->shape()[2];

    if(embIdx.empty()) {
      stateHardAtt->setAttentionIndices({0});
    } else {
      for(size_t i = 0; i < embIdx.size(); ++i)
        if(specialSymbols_.count(embIdx[i])) {
          stateHardAtt->getAttentionIndices()[i]++;
          if(stateHardAtt->getAttentionIndices()[i] >= dimSrcWords)
            stateHardAtt->getAttentionIndices()[i] = dimSrcWords - 1;
        }
    }
  }

  const std::vector<Expr> getAlignments() { return {}; }
};

typedef EncoderDecoder<EncoderS2S, DecoderHardAtt> HardAtt;

/******************************************************************************/

class DecoderHardSoftAtt : public DecoderHardAtt {
private:
  Ptr<rnn::RNN> rnn_;

public:
  template <class... Args>
  DecoderHardSoftAtt(Ptr<Config> options, Args... args)
      : DecoderHardAtt(options, args...) {}

  virtual Ptr<DecoderState> step(Ptr<ExpressionGraph> graph,
                                 Ptr<DecoderState> state) {
    using namespace keywords;

    int dimTrgVoc = options_->get<std::vector<int>>("dim-vocabs").back();

    int dimTrgEmb = options_->get<int>("dim-emb");

    int dimDecState = options_->get<int>("dim-rnn");
    bool layerNorm = options_->get<bool>("layer-normalization");
    bool skipDepth = options_->get<bool>("skip");

    size_t decoderLayers = options_->get<size_t>("dec-depth");
    auto cellType = options_->get<std::string>("dec-cell");

    float dropoutRnn = inference_ ? 0 : options_->get<float>("dropout-rnn");
    float dropoutTrg = inference_ ? 0 : options_->get<float>("dropout-trg");

    auto stateHardAtt = std::dynamic_pointer_cast<DecoderStateHardAtt>(state);

    auto trgEmbeddings = stateHardAtt->getTargetEmbeddings();

    auto context = stateHardAtt->getEncoderState()->getContext();
    int dimContext = context->shape()[1];
    int dimSrcWords = context->shape()[2];

    int dimBatch = context->shape()[0];
    int dimTrgWords = trgEmbeddings->shape()[2];
    int dimBeam = trgEmbeddings->shape()[3];

    if(dropoutTrg) {
      auto trgWordDrop = graph->dropout(dropoutTrg, {dimBatch, 1, dimTrgWords});
      trgEmbeddings = dropout(trgEmbeddings, mask = trgWordDrop);
    }

    auto flatContext = reshape(context, {dimBatch * dimSrcWords, dimContext});
    auto attendedContext
        = rows(flatContext, stateHardAtt->getAttentionIndices());
    attendedContext = reshape(attendedContext,
                              {dimBatch, dimContext, dimTrgWords, dimBeam});

    auto rnnInputs = concatenate({trgEmbeddings, attendedContext}, axis = 1);
    int dimInput = rnnInputs->shape()[1];

    if(!rnn_) {

      auto rnn = rnn::rnn(graph)
                 ("type", cellType)
                 ("dimInput", dimInput)
                 ("dimState", dimDecState)
                 ("dropout", dropoutRnn)
                 ("layer-normalization", layerNorm)
                 ("skip", skipDepth);

      auto attCell = rnn::stacked_cell(graph)
                     .push_back(rnn::cell(graph)
                                ("prefix", prefix_ + "_cell1"))
                     .push_back(rnn::attention(graph)
                                ("prefix", prefix_)
                                .set_state(state->getEncoderState()))
                     .push_back(rnn::cell(graph)
                                ("prefix", prefix_ + "_cell2")
                                ("final", true));

      rnn.push_back(attCell);
      for(int i = 0; i < decoderLayers - 1; ++i)
        rnn.push_back(rnn::cell(graph)
                      ("prefix", prefix_ + "_l" + std::to_string(i)));

      rnn_ = rnn.construct();

    }

    auto decContext = rnn_->transduce(rnnInputs, stateHardAtt->getStates());
    rnn::States decStates = rnn_->lastCellStates();

    bool single = stateHardAtt->doSingleStep();
    auto att = rnn_->at(0)->as<rnn::StackedCell>()->at(1)->as<rnn::Attention>();
    auto alignedContext = single ? att->getContexts().back() :
                                   concatenate(att->getContexts(),
                                               keywords::axis = 2);

    //// 2-layer feedforward network for outputs and cost
    auto out = mlp::mlp(graph)
               .push_back(mlp::dense(graph)
                          ("prefix", prefix_ + "_ff_logit_l1")
                          ("dim", dimTrgEmb)
                          ("activation", mlp::act::tanh)
                          ("layer-normalization", layerNorm))
               .push_back(mlp::dense(graph)
                          ("prefix", prefix_ + "_ff_logit_l2")
                          ("dim", dimTrgVoc));

    auto logits = out->apply(rnnInputs, decContext, alignedContext);

    return New<DecoderStateHardAtt>(decStates,
                                    logits,
                                    stateHardAtt->getEncoderState(),
                                    stateHardAtt->getAttentionIndices());
  }

  const std::vector<Expr> getAlignments() {
    auto att = rnn_->at(0)->as<rnn::StackedCell>()->at(1)->as<rnn::Attention>();
    return att->getAlignments();
  }
};

typedef EncoderDecoder<EncoderS2S, DecoderHardSoftAtt> HardSoftAtt;

/*
class MultiDecoderHardSoftAtt : public DecoderHardSoftAtt {
private:
  Ptr<GlobalAttention> attention1_;
  Ptr<GlobalAttention> attention2_;
  Ptr<RNN<MultiCGRU>> rnnL1;
  Ptr<MLRNN<GRU>> rnnLn;

public:
  template <class... Args>
  MultiDecoderHardSoftAtt(Ptr<Config> options, Args... args)
      : DecoderHardSoftAtt(options, args...) {}

  virtual Ptr<DecoderState> startState(Ptr<EncoderState> encState) {
    using namespace keywords;

    auto mEncState = std::static_pointer_cast<EncoderStateMultiS2S>(encState);

    auto meanContext1 = weighted_average(
        mEncState->enc1->getContext(), mEncState->enc1->getMask(), axis = 2);

    auto meanContext2 = weighted_average(
        mEncState->enc2->getContext(), mEncState->enc2->getMask(), axis = 2);

    bool layerNorm = options_->get<bool>("layer-normalization");

    auto start = Dense("ff_state",
                       options_->get<int>("dim-rnn"),
                       activation = mlp::act::tanh,
                       layer-normalization = layerNorm)(meanContext1, meanContext2);

    std::vector<Expr> startStates(options_->get<size_t>("layers-dec"), start);
    return New<DecoderStateHardAtt>(
        startStates, nullptr, encState, std::vector<size_t>({0}));
  }

  virtual Ptr<DecoderState> step(Ptr<ExpressionGraph> graph,
                                 Ptr<DecoderState> state) {
    using namespace keywords;

    int dimTrgVoc = options_->get<std::vector<int>>("dim-vocabs").back();

    int dimTrgEmb
        = options_->get<int>("dim-emb") + options_->get<int>("dim-pos");

    int dimDecState = options_->get<int>("dim-rnn");
    bool layerNorm = options_->get<bool>("layer-normalization");
    bool skipDepth = options_->get<bool>("skip");
    size_t decoderLayers = options_->get<size_t>("layers-dec");

    float dropoutRnn = inference_ ? 0 : options_->get<float>("dropout-rnn");
    float dropoutTrg = inference_ ? 0 : options_->get<float>("dropout-trg");

    auto mEncState = std::static_pointer_cast<EncoderStateMultiS2S>(
        state->getEncoderState());

    auto stateHardAtt = std::dynamic_pointer_cast<DecoderStateHardAtt>(state);

    auto trgEmbeddings = stateHardAtt->getTargetEmbeddings();

    auto context = mEncState->enc1->getContext();
    int dimContext = context->shape()[1];
    int dimSrcWords = context->shape()[2];

    int dimBatch = context->shape()[0];
    int dimTrgWords = trgEmbeddings->shape()[2];
    int dimBeam = trgEmbeddings->shape()[3];

    if(dropoutTrg) {
      auto trgWordDrop = graph->dropout(dropoutTrg, {dimBatch, 1, dimTrgWords});
      trgEmbeddings = dropout(trgEmbeddings, mask = trgWordDrop);
    }

    auto flatContext = reshape(context, {dimBatch * dimSrcWords, dimContext});
    auto attendedContext
        = rows(flatContext, stateHardAtt->getAttentionIndices());
    attendedContext = reshape(attendedContext,
                              {dimBatch, dimContext, dimTrgWords, dimBeam});

    auto rnnInputs = concatenate({trgEmbeddings, attendedContext}, axis = 1);
    int dimInput = rnnInputs->shape()[1];

    if(!attention1_)
      attention1_ = New<GlobalAttention>(
          "decoder_att1", mEncState->enc1, dimDecState, layer-normalization = layerNorm);
    if(!attention2_)
      attention2_ = New<GlobalAttention>(
          "decoder_att2", mEncState->enc2, dimDecState, layer-normalization = layerNorm);

    if(!rnnL1)
      rnnL1 = New<RNN<MultiCGRU>>(graph,
                                  "decoder",
                                  dimInput,
                                  dimDecState,
                                  attention1_,
                                  attention2_,
                                  dropout_prob = dropoutRnn,
                                  layer-normalization = layerNorm);

    auto stateL1 = (*rnnL1)(rnnInputs, stateHardAtt->getStates()[0]);

    bool single = stateHardAtt->doSingleStep();
    auto alignedContext = single ? rnnL1->getCell()->getLastContext() :
                                   rnnL1->getCell()->getContexts();

    std::vector<Expr> statesOut;
    statesOut.push_back(stateL1);

    Expr outputLn;
    if(decoderLayers > 1) {
      std::vector<Expr> statesIn;
      for(int i = 1; i < stateHardAtt->getStates().size(); ++i)
        statesIn.push_back(stateHardAtt->getStates()[i]);

      if(!rnnLn)
        rnnLn = New<MLRNN<GRU>>(graph,
                                "decoder",
                                decoderLayers - 1,
                                dimDecState,
                                dimDecState,
                                layer-normalization = layerNorm,
                                dropout_prob = dropoutRnn,
                                skip = skipDepth,
                                skip_first = skipDepth);

      std::vector<Expr> statesLn;
      std::tie(outputLn, statesLn) = (*rnnLn)(stateL1, statesIn);

      statesOut.insert(statesOut.end(), statesLn.begin(), statesLn.end());
    } else {
      outputLn = stateL1;
    }

    //// 2-layer feedforward network for outputs and cost
    auto logitsL1
        = Dense("ff_logit_l1",
                dimTrgEmb,
                activation = mlp::act::tanh,
                layer-normalization = layerNorm)(rnnInputs, outputLn, alignedContext);

    auto logitsOut = Dense("ff_logit_l2", dimTrgVoc)(logitsL1);

    return New<DecoderStateHardAtt>(statesOut,
                                    logitsOut,
                                    stateHardAtt->getEncoderState(),
                                    stateHardAtt->getAttentionIndices());
  }

  virtual void selectEmbeddings(Ptr<ExpressionGraph> graph,
                                Ptr<DecoderState> state,
                                const std::vector<size_t>& embIdx) {
    DecoderBase::selectEmbeddings(graph, state, embIdx);

    auto stateHardAtt = std::dynamic_pointer_cast<DecoderStateHardAtt>(state);

    auto mEncState = std::static_pointer_cast<EncoderStateMultiS2S>(
        state->getEncoderState());

    int dimSrcWords = mEncState->enc1->getContext()->shape()[2];

    if(embIdx.empty()) {
      stateHardAtt->setAttentionIndices({0});
    } else {
      for(size_t i = 0; i < embIdx.size(); ++i)
        if(specialSymbols_.count(embIdx[i])) {
          stateHardAtt->getAttentionIndices()[i]++;
          if(stateHardAtt->getAttentionIndices()[i] >= dimSrcWords)
            stateHardAtt->getAttentionIndices()[i] = dimSrcWords - 1;
        }
    }
  }

  const std::vector<Expr> getAlignments() {
    return attention1_->getAlignments();
  }
};

class MultiHardSoftAtt
    : public EncoderDecoder<MultiEncoderS2S, MultiDecoderHardSoftAtt> {
public:
  template <class... Args>
  MultiHardSoftAtt(Ptr<Config> options, Args... args)
      : EncoderDecoder(options, {0, 1, 2}, args...) {}
};
*/

}
