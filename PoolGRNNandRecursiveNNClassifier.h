
#ifndef SRC_PoolGRNNandRecursiveNNClassifier_H_
#define SRC_PoolGRNNandRecursiveNNClassifier_H_

#include <iostream>

#include <assert.h>
#include "Example.h"
#include "Feature.h"
#include "N3L.h"
#include "SemiDepRecursiveNN.h"

using namespace nr;
using namespace std;
using namespace mshadow;
using namespace mshadow::expr;
using namespace mshadow::utils;

//A native neural network classfier using only word embeddings
template<typename xpu>
class PoolGRNNandRecursiveNNClassifier {
public:
	PoolGRNNandRecursiveNNClassifier() {
    _dropOut = 0.5;
  }
  ~PoolGRNNandRecursiveNNClassifier() {

  }

public:
  LookupTable<xpu> _words;
  LookupTable<xpu> _pos;
  LookupTable<xpu> _sst;
  LookupTable<xpu> _ner;

  int _wordcontext, _wordwindow;
  int _wordSize;
  int _wordDim;

  int _token_representation_size;
  int _inputsize;
  int _hiddensize;
  int _rnnHiddenSize;

  UniLayer<xpu> _olayer_linear;
  UniLayer<xpu> _tanh_project;

  GRNN<xpu> _rnn_left;
  GRNN<xpu> _rnn_right;
  SemiDepRecursiveNN<xpu> _recursive;

  int _poolmanners;
  int _poolfunctions;

  int _poolsize;

  int _labelSize;

  Metric _eval;

  dtype _dropOut;

  int _remove; // 1, avg, 2, max, 3, min, 4, std, 5, pro

  Options options;

  int _poolInputSize;

public:

  inline void init(const NRMat<dtype>& wordEmb, Options options) {
	  this->options = options;
    _wordcontext = options.wordcontext;
    _wordwindow = 2 * _wordcontext + 1;
    _wordSize = wordEmb.nrows();
    _wordDim = wordEmb.ncols();

    _labelSize = MAX_RELATION;
    _token_representation_size = _wordDim;
    _poolfunctions = 5;
    _poolmanners = _poolfunctions * 5; //( left, right, target) * (avg, max, min, std, pro)
    _inputsize = _wordwindow * _token_representation_size;
    _hiddensize = options.wordEmbSize;
    _rnnHiddenSize = options.rnnHiddenSize;
    _poolInputSize = 2*_hiddensize;

    _poolsize = _poolmanners * _poolInputSize;

    _words.initial(wordEmb);

    _rnn_left.initial(_rnnHiddenSize, _inputsize, true, 10);
    _rnn_right.initial(_rnnHiddenSize, _inputsize, false, 40);

    _recursive.initial(_hiddensize, _inputsize, 20);

    _tanh_project.initial(_hiddensize, 2*_rnnHiddenSize, true, 70, 0);
    _olayer_linear.initial(_labelSize, _poolsize, false, 80, 2);

    _remove = 0;

    cout<<"PoolGRNNandRecursiveNNClassifier initial"<<endl;
  }

  inline void release() {
    _words.release();
    _sst.release();
    _ner.release();
    _pos.release();

    _olayer_linear.release();
    _tanh_project.release();
    _rnn_left.release();
    _rnn_right.release();
    _recursive.release();

  }

  inline dtype process(const vector<Example>& examples, int iter) {
    _eval.reset();

    int example_num = examples.size();
    dtype cost = 0.0;
    int offset = 0;

    for (int count = 0; count < example_num; count++) {
      const Example& example = examples[count];

      int seq_size = example.m_features.size();
      int sentNum = example.dep.size();

      Tensor<xpu, 3, dtype> input, inputLoss;
      Tensor<xpu, 3, dtype> project, projectLoss;

      Tensor<xpu, 3, dtype> rnn_hidden_left, rnn_hidden_leftLoss;
      Tensor<xpu, 3, dtype> rnn_hidden_left_reset, rnn_hidden_left_afterreset, rnn_hidden_left_update, rnn_hidden_left_current;
      Tensor<xpu, 3, dtype> rnn_hidden_right, rnn_hidden_rightLoss;
      Tensor<xpu, 3, dtype> rnn_hidden_right_reset, rnn_hidden_right_afterreset, rnn_hidden_right_update, rnn_hidden_right_current;

      Tensor<xpu, 3, dtype> rnn_hidden_merge, rnn_hidden_mergeLoss;

      vector< Tensor<xpu, 3, dtype> > v_recursive_input(sentNum), v_recursive_input_loss(sentNum);
      vector< Tensor<xpu, 3, dtype> > v_recursive_rsp(sentNum);
      vector< vector< Tensor<xpu, 3, dtype> > > v_recursive_v_rsc(sentNum);
      vector< Tensor<xpu, 3, dtype> > v_recursive_hidden(sentNum), v_recursive_hidden_loss(sentNum);

      Tensor<xpu, 3, dtype> poolInput, poolInputLoss;

      vector<Tensor<xpu, 2, dtype> > pool(_poolmanners), poolLoss(_poolmanners);
      vector<Tensor<xpu, 3, dtype> > poolIndex(_poolmanners);

      Tensor<xpu, 2, dtype> poolmerge, poolmergeLoss;
      Tensor<xpu, 2, dtype> output, outputLoss;

      Tensor<xpu, 3, dtype> wordprime, wordprimeLoss, wordprimeMask;
      Tensor<xpu, 3, dtype> wordrepresent, wordrepresentLoss;

      hash_set<int> beforeIndex, formerIndex, middleIndex, latterIndex, afterIndex;
      Tensor<xpu, 2, dtype> beforerepresent, beforerepresentLoss;
      Tensor<xpu, 2, dtype> formerrepresent, formerrepresentLoss;
	  Tensor<xpu, 2, dtype> middlerepresent, middlerepresentLoss;
      Tensor<xpu, 2, dtype> latterrepresent, latterrepresentLoss;
	  Tensor<xpu, 2, dtype> afterrepresent, afterrepresentLoss;

      //initialize
      wordprime = NewTensor<xpu>(Shape3(seq_size, 1, _wordDim), 0.0);
      wordprimeLoss = NewTensor<xpu>(Shape3(seq_size, 1, _wordDim), 0.0);
      wordprimeMask = NewTensor<xpu>(Shape3(seq_size, 1, _wordDim), 1.0);
      wordrepresent = NewTensor<xpu>(Shape3(seq_size, 1, _token_representation_size), 0.0);
      wordrepresentLoss = NewTensor<xpu>(Shape3(seq_size, 1, _token_representation_size), 0.0);

      input = NewTensor<xpu>(Shape3(seq_size, 1, _inputsize), 0.0);
      inputLoss = NewTensor<xpu>(Shape3(seq_size, 1, _inputsize), 0.0);

      rnn_hidden_left_reset = NewTensor<xpu>(Shape3(seq_size, 1, _rnnHiddenSize), 0.0);
      rnn_hidden_left_update = NewTensor<xpu>(Shape3(seq_size, 1, _rnnHiddenSize), 0.0);
      rnn_hidden_left_afterreset = NewTensor<xpu>(Shape3(seq_size, 1, _rnnHiddenSize), 0.0);
      rnn_hidden_left_current = NewTensor<xpu>(Shape3(seq_size, 1, _rnnHiddenSize), 0.0);
      rnn_hidden_left = NewTensor<xpu>(Shape3(seq_size, 1, _rnnHiddenSize), 0.0);
      rnn_hidden_leftLoss = NewTensor<xpu>(Shape3(seq_size, 1, _rnnHiddenSize), 0.0);

      rnn_hidden_right_reset = NewTensor<xpu>(Shape3(seq_size, 1, _rnnHiddenSize), 0.0);
      rnn_hidden_right_update = NewTensor<xpu>(Shape3(seq_size, 1, _rnnHiddenSize), 0.0);
      rnn_hidden_right_afterreset = NewTensor<xpu>(Shape3(seq_size, 1, _rnnHiddenSize), 0.0);
      rnn_hidden_right_current = NewTensor<xpu>(Shape3(seq_size, 1, _rnnHiddenSize), 0.0);
      rnn_hidden_right = NewTensor<xpu>(Shape3(seq_size, 1, _rnnHiddenSize), 0.0);
      rnn_hidden_rightLoss = NewTensor<xpu>(Shape3(seq_size, 1, _rnnHiddenSize), 0.0);

      rnn_hidden_merge = NewTensor<xpu>(Shape3(seq_size, 1, 2*_rnnHiddenSize), 0.0);
      rnn_hidden_mergeLoss = NewTensor<xpu>(Shape3(seq_size, 1, 2*_rnnHiddenSize), 0.0);

      project = NewTensor<xpu>(Shape3(seq_size, 1, _hiddensize), 0.0);
      projectLoss = NewTensor<xpu>(Shape3(seq_size, 1, _hiddensize), 0.0);


      for(int sentIdx=0;sentIdx<sentNum;sentIdx++) {
    	  int sentLength = sentIdx==0 ? example.sentEnd[sentIdx] : example.sentEnd[sentIdx]-example.sentEnd[sentIdx-1];
    	  v_recursive_input[sentIdx] = NewTensor<xpu>(Shape3(sentLength, 1, _inputsize), 0.0);
    	  v_recursive_input_loss[sentIdx] = NewTensor<xpu>(Shape3(sentLength, 1, _inputsize), 0.0);
    	  v_recursive_rsp[sentIdx] = NewTensor<xpu>(Shape3(sentLength, 1, _hiddensize), 0.0);
    	  v_recursive_hidden[sentIdx] = NewTensor<xpu>(Shape3(sentLength, 1, _hiddensize), 0.0);
    	  v_recursive_hidden_loss[sentIdx] = NewTensor<xpu>(Shape3(sentLength, 1, _hiddensize), 0.0);
      }

      poolInput = NewTensor<xpu>(Shape3(seq_size, 1, _poolInputSize), 0.0);
      poolInputLoss = NewTensor<xpu>(Shape3(seq_size, 1, _poolInputSize), 0.0);

      for (int idm = 0; idm < _poolmanners; idm++) {
        pool[idm] = NewTensor<xpu>(Shape2(1, _poolInputSize), 0.0);
        poolLoss[idm] = NewTensor<xpu>(Shape2(1, _poolInputSize), 0.0);
        poolIndex[idm] = NewTensor<xpu>(Shape3(seq_size, 1, _poolInputSize), 0.0);
      }

      beforerepresent = NewTensor<xpu>(Shape2(1, _poolfunctions * _poolInputSize), 0.0);
      beforerepresentLoss = NewTensor<xpu>(Shape2(1, _poolfunctions * _poolInputSize), 0.0);

      formerrepresent = NewTensor<xpu>(Shape2(1, _poolfunctions * _poolInputSize), 0.0);
      formerrepresentLoss = NewTensor<xpu>(Shape2(1, _poolfunctions * _poolInputSize), 0.0);

      middlerepresent = NewTensor<xpu>(Shape2(1, _poolfunctions * _poolInputSize), 0.0);
      middlerepresentLoss = NewTensor<xpu>(Shape2(1, _poolfunctions * _poolInputSize), 0.0);

	  latterrepresent = NewTensor<xpu>(Shape2(1, _poolfunctions * _poolInputSize), 0.0);
      latterrepresentLoss = NewTensor<xpu>(Shape2(1, _poolfunctions * _poolInputSize), 0.0);

	  afterrepresent = NewTensor<xpu>(Shape2(1, _poolfunctions * _poolInputSize), 0.0);
      afterrepresentLoss = NewTensor<xpu>(Shape2(1, _poolfunctions * _poolInputSize), 0.0);

      poolmerge = NewTensor<xpu>(Shape2(1, _poolsize), 0.0);
      poolmergeLoss = NewTensor<xpu>(Shape2(1, _poolsize), 0.0);
      output = NewTensor<xpu>(Shape2(1, _labelSize), 0.0);
      outputLoss = NewTensor<xpu>(Shape2(1, _labelSize), 0.0);
      //forward propagation
      //input setting, and linear setting
      for (int idx = 0; idx < seq_size; idx++) {
        const Feature& feature = example.m_features[idx];
        //linear features should not be dropped out

        srand(iter * example_num + count * seq_size + idx);

        const vector<int>& words = feature.words;
        if (idx < example.formerTkBegin) {
          beforeIndex.insert(idx);
        } else if(idx >= example.formerTkBegin && idx <= example.formerTkEnd) {
			formerIndex.insert(idx);
		} else if (idx >= example.latterTkBegin && idx <= example.latterTkEnd) {
          latterIndex.insert(idx);
        } else if (idx > example.latterTkEnd) {
          afterIndex.insert(idx);
        } else {
          middleIndex.insert(idx);
        }

       _words.GetEmb(words[0], wordprime[idx]);

        //dropout
        dropoutcol(wordprimeMask[idx], _dropOut);
        wordprime[idx] = wordprime[idx] * wordprimeMask[idx];
      }

      for (int idx = 0; idx < seq_size; idx++) {
        wordrepresent[idx] += wordprime[idx];
      }

      windowlized(wordrepresent, input, _wordcontext);

      _rnn_left.ComputeForwardScore(input, rnn_hidden_left_reset, rnn_hidden_left_afterreset, rnn_hidden_left_update, rnn_hidden_left_current, rnn_hidden_left);
      _rnn_right.ComputeForwardScore(input, rnn_hidden_right_reset, rnn_hidden_right_afterreset, rnn_hidden_right_update, rnn_hidden_right_current, rnn_hidden_right);


      for (int idx = 0; idx < seq_size; idx++) {
        concat(rnn_hidden_left[idx], rnn_hidden_right[idx], rnn_hidden_merge[idx]);
      }

      // do we need a convolution? future work, currently needn't
      for (int idx = 0; idx < seq_size; idx++) {
        _tanh_project.ComputeForwardScore(rnn_hidden_merge[idx], project[idx]);
      }

      int sentBegin = 0;
      for(int sentIdx=0;sentIdx<sentNum;sentIdx++) {
    	  int sentLength = sentIdx==0 ? example.sentEnd[sentIdx] : example.sentEnd[sentIdx]-example.sentEnd[sentIdx-1];
    	  int seqIdx = sentBegin;
    	  for(int idx=0;idx<sentLength;idx++, seqIdx++) {
    		  v_recursive_input[sentIdx][idx] += input[seqIdx];
    	  }

          _recursive.ComputeForwardScore(v_recursive_input[sentIdx], example.dep[sentIdx], example.depType[sentIdx],
        		  v_recursive_v_rsc[sentIdx], v_recursive_rsp[sentIdx],
        		  v_recursive_hidden[sentIdx]);

          seqIdx = sentBegin;
    	  for(int idx=0;idx<sentLength;idx++, seqIdx++) {
    		  concat(project[seqIdx], v_recursive_hidden[sentIdx][idx], poolInput[seqIdx]);
    	  }

    	  sentBegin += sentLength;
      }


      offset = 0;
      //before
      //avg pooling
      if ((_remove > 0 && _remove != 1) || (_remove < 0 && _remove == -1) || _remove == 0) {
        avgpool_forward(poolInput, pool[offset], poolIndex[offset], beforeIndex);
      }
      //max pooling
      if ((_remove > 0 && _remove != 2) || (_remove < 0 && _remove == -2) || _remove == 0) {
        maxpool_forward(poolInput, pool[offset + 1], poolIndex[offset + 1], beforeIndex);
      }
      //min pooling
      if ((_remove > 0 && _remove != 3) || (_remove < 0 && _remove == -3) || _remove == 0) {
        minpool_forward(poolInput, pool[offset + 2], poolIndex[offset + 2], beforeIndex);
      }
      //std pooling
      if ((_remove > 0 && _remove != 4) || (_remove < 0 && _remove == -4) || _remove == 0) {
        stdpool_forward(poolInput, pool[offset + 3], poolIndex[offset + 3], beforeIndex);
      }
      //pro pooling
      if ((_remove > 0 && _remove != 5) || (_remove < 0 && _remove == -5) || _remove == 0) {
        propool_forward(poolInput, pool[offset + 4], poolIndex[offset + 4], beforeIndex);
      }

      concat(pool[offset], pool[offset + 1], pool[offset + 2], pool[offset + 3], pool[offset + 4], beforerepresent);

      offset = _poolfunctions;
      //former
      //avg pooling
      if ((_remove > 0 && _remove != 1) || (_remove < 0 && _remove == -1) || _remove == 0) {
        avgpool_forward(poolInput, pool[offset], poolIndex[offset], formerIndex);
      }
      //max pooling
      if ((_remove > 0 && _remove != 2) || (_remove < 0 && _remove == -2) || _remove == 0) {
        maxpool_forward(poolInput, pool[offset + 1], poolIndex[offset + 1], formerIndex);
      }
      //min pooling
      if ((_remove > 0 && _remove != 3) || (_remove < 0 && _remove == -3) || _remove == 0) {
        minpool_forward(poolInput, pool[offset + 2], poolIndex[offset + 2], formerIndex);
      }
      //std pooling
      if ((_remove > 0 && _remove != 4) || (_remove < 0 && _remove == -4) || _remove == 0) {
        stdpool_forward(poolInput, pool[offset + 3], poolIndex[offset + 3], formerIndex);
      }
      //pro pooling
      if ((_remove > 0 && _remove != 5) || (_remove < 0 && _remove == -5) || _remove == 0) {
        propool_forward(poolInput, pool[offset + 4], poolIndex[offset + 4], formerIndex);
      }

      concat(pool[offset], pool[offset + 1], pool[offset + 2], pool[offset + 3], pool[offset + 4], formerrepresent);

      offset = 2 * _poolfunctions;
      //middle
      //avg pooling
      if ((_remove > 0 && _remove != 1) || (_remove < 0 && _remove == -1) || _remove == 0) {
        avgpool_forward(poolInput, pool[offset], poolIndex[offset], middleIndex);
      }
      //max pooling
      if ((_remove > 0 && _remove != 2) || (_remove < 0 && _remove == -2) || _remove == 0) {
        maxpool_forward(poolInput, pool[offset + 1], poolIndex[offset + 1], middleIndex);
      }
      //min pooling
      if ((_remove > 0 && _remove != 3) || (_remove < 0 && _remove == -3) || _remove == 0) {
        minpool_forward(poolInput, pool[offset + 2], poolIndex[offset + 2], middleIndex);
      }
      //std pooling
      if ((_remove > 0 && _remove != 4) || (_remove < 0 && _remove == -4) || _remove == 0) {
        stdpool_forward(poolInput, pool[offset + 3], poolIndex[offset + 3], middleIndex);
      }
      //pro pooling
      if ((_remove > 0 && _remove != 5) || (_remove < 0 && _remove == -5) || _remove == 0) {
        propool_forward(poolInput, pool[offset + 4], poolIndex[offset + 4], middleIndex);
      }

      concat(pool[offset], pool[offset + 1], pool[offset + 2], pool[offset + 3], pool[offset + 4], middlerepresent);

	  offset = 3 * _poolfunctions;
      //latter
      //avg pooling
      if ((_remove > 0 && _remove != 1) || (_remove < 0 && _remove == -1) || _remove == 0) {
        avgpool_forward(poolInput, pool[offset], poolIndex[offset], latterIndex);
      }
      //max pooling
      if ((_remove > 0 && _remove != 2) || (_remove < 0 && _remove == -2) || _remove == 0) {
        maxpool_forward(poolInput, pool[offset + 1], poolIndex[offset + 1], latterIndex);
      }
      //min pooling
      if ((_remove > 0 && _remove != 3) || (_remove < 0 && _remove == -3) || _remove == 0) {
        minpool_forward(poolInput, pool[offset + 2], poolIndex[offset + 2], latterIndex);
      }
      //std pooling
      if ((_remove > 0 && _remove != 4) || (_remove < 0 && _remove == -4) || _remove == 0) {
        stdpool_forward(poolInput, pool[offset + 3], poolIndex[offset + 3], latterIndex);
      }
      //pro pooling
      if ((_remove > 0 && _remove != 5) || (_remove < 0 && _remove == -5) || _remove == 0) {
        propool_forward(poolInput, pool[offset + 4], poolIndex[offset + 4], latterIndex);
      }

      concat(pool[offset], pool[offset + 1], pool[offset + 2], pool[offset + 3], pool[offset + 4], latterrepresent);

	  offset = 4 * _poolfunctions;
      //after
      //avg pooling
      if ((_remove > 0 && _remove != 1) || (_remove < 0 && _remove == -1) || _remove == 0) {
        avgpool_forward(poolInput, pool[offset], poolIndex[offset], afterIndex);
      }
      //max pooling
      if ((_remove > 0 && _remove != 2) || (_remove < 0 && _remove == -2) || _remove == 0) {
        maxpool_forward(poolInput, pool[offset + 1], poolIndex[offset + 1], afterIndex);
      }
      //min pooling
      if ((_remove > 0 && _remove != 3) || (_remove < 0 && _remove == -3) || _remove == 0) {
        minpool_forward(poolInput, pool[offset + 2], poolIndex[offset + 2], afterIndex);
      }
      //std pooling
      if ((_remove > 0 && _remove != 4) || (_remove < 0 && _remove == -4) || _remove == 0) {
        stdpool_forward(poolInput, pool[offset + 3], poolIndex[offset + 3], afterIndex);
      }
      //pro pooling
      if ((_remove > 0 && _remove != 5) || (_remove < 0 && _remove == -5) || _remove == 0) {
        propool_forward(poolInput, pool[offset + 4], poolIndex[offset + 4], afterIndex);
      }

      concat(pool[offset], pool[offset + 1], pool[offset + 2], pool[offset + 3], pool[offset + 4], afterrepresent);

      concat(beforerepresent, formerrepresent, middlerepresent, latterrepresent, afterrepresent, poolmerge);

      _olayer_linear.ComputeForwardScore(poolmerge, output);

      // get delta for each output
      cost += softmax_loss(output, example.m_labels, outputLoss, _eval, example_num);

      // loss backward propagation
      _olayer_linear.ComputeBackwardLoss(poolmerge, output, outputLoss, poolmergeLoss);

      unconcat(beforerepresentLoss, formerrepresentLoss, middlerepresentLoss, latterrepresentLoss, afterrepresentLoss, poolmergeLoss);


      offset = 0;
      //before
      unconcat(poolLoss[offset], poolLoss[offset + 1], poolLoss[offset + 2], poolLoss[offset + 3], poolLoss[offset + 4], beforerepresentLoss);

      //avg pooling
      if ((_remove > 0 && _remove != 1) || (_remove < 0 && _remove == -1) || _remove == 0) {
        pool_backward(poolLoss[offset], poolIndex[offset],  poolInputLoss);
      }
      //max pooling
      if ((_remove > 0 && _remove != 2) || (_remove < 0 && _remove == -2) || _remove == 0) {
        pool_backward(poolLoss[offset + 1], poolIndex[offset + 1], poolInputLoss);
      }
      //min pooling
      if ((_remove > 0 && _remove != 3) || (_remove < 0 && _remove == -3) || _remove == 0) {
        pool_backward(poolLoss[offset + 2], poolIndex[offset + 2], poolInputLoss);
      }
      //std pooling
      if ((_remove > 0 && _remove != 4) || (_remove < 0 && _remove == -4) || _remove == 0) {
        pool_backward(poolLoss[offset + 3], poolIndex[offset + 3], poolInputLoss);
      }
      //pro pooling
      if ((_remove > 0 && _remove != 5) || (_remove < 0 && _remove == -5) || _remove == 0) {
        pool_backward(poolLoss[offset + 4], poolIndex[offset + 4], poolInputLoss);
      }

      offset = _poolfunctions;
      //former
      unconcat(poolLoss[offset], poolLoss[offset + 1], poolLoss[offset + 2], poolLoss[offset + 3], poolLoss[offset + 4], formerrepresentLoss);

      //avg pooling
      if ((_remove > 0 && _remove != 1) || (_remove < 0 && _remove == -1) || _remove == 0) {
        pool_backward(poolLoss[offset], poolIndex[offset],  poolInputLoss);
      }
      //max pooling
      if ((_remove > 0 && _remove != 2) || (_remove < 0 && _remove == -2) || _remove == 0) {
        pool_backward(poolLoss[offset + 1], poolIndex[offset + 1], poolInputLoss);
      }
      //min pooling
      if ((_remove > 0 && _remove != 3) || (_remove < 0 && _remove == -3) || _remove == 0) {
        pool_backward(poolLoss[offset + 2], poolIndex[offset + 2], poolInputLoss);
      }
      //std pooling
      if ((_remove > 0 && _remove != 4) || (_remove < 0 && _remove == -4) || _remove == 0) {
        pool_backward(poolLoss[offset + 3], poolIndex[offset + 3], poolInputLoss);
      }
      //pro pooling
      if ((_remove > 0 && _remove != 5) || (_remove < 0 && _remove == -5) || _remove == 0) {
        pool_backward(poolLoss[offset + 4], poolIndex[offset + 4], poolInputLoss);
      }

      offset = 2 * _poolfunctions;
      //middle
      unconcat(poolLoss[offset], poolLoss[offset + 1], poolLoss[offset + 2], poolLoss[offset + 3], poolLoss[offset + 4], middlerepresentLoss);

      //avg pooling
      if ((_remove > 0 && _remove != 1) || (_remove < 0 && _remove == -1) || _remove == 0) {
        pool_backward(poolLoss[offset], poolIndex[offset],  poolInputLoss);
      }
      //max pooling
      if ((_remove > 0 && _remove != 2) || (_remove < 0 && _remove == -2) || _remove == 0) {
        pool_backward(poolLoss[offset + 1], poolIndex[offset + 1], poolInputLoss);
      }
      //min pooling
      if ((_remove > 0 && _remove != 3) || (_remove < 0 && _remove == -3) || _remove == 0) {
        pool_backward(poolLoss[offset + 2], poolIndex[offset + 2], poolInputLoss);
      }
      //std pooling
      if ((_remove > 0 && _remove != 4) || (_remove < 0 && _remove == -4) || _remove == 0) {
        pool_backward(poolLoss[offset + 3], poolIndex[offset + 3], poolInputLoss);
      }
      //pro pooling
      if ((_remove > 0 && _remove != 5) || (_remove < 0 && _remove == -5) || _remove == 0) {
        pool_backward(poolLoss[offset + 4], poolIndex[offset + 4], poolInputLoss);
      }

	  offset = 3 * _poolfunctions;
      //latter
      unconcat(poolLoss[offset], poolLoss[offset + 1], poolLoss[offset + 2], poolLoss[offset + 3], poolLoss[offset + 4], latterrepresentLoss);

      //avg pooling
      if ((_remove > 0 && _remove != 1) || (_remove < 0 && _remove == -1) || _remove == 0) {
        pool_backward(poolLoss[offset], poolIndex[offset],  poolInputLoss);
      }
      //max pooling
      if ((_remove > 0 && _remove != 2) || (_remove < 0 && _remove == -2) || _remove == 0) {
        pool_backward(poolLoss[offset + 1], poolIndex[offset + 1], poolInputLoss);
      }
      //min pooling
      if ((_remove > 0 && _remove != 3) || (_remove < 0 && _remove == -3) || _remove == 0) {
        pool_backward(poolLoss[offset + 2], poolIndex[offset + 2], poolInputLoss);
      }
      //std pooling
      if ((_remove > 0 && _remove != 4) || (_remove < 0 && _remove == -4) || _remove == 0) {
        pool_backward(poolLoss[offset + 3], poolIndex[offset + 3], poolInputLoss);
      }
      //pro pooling
      if ((_remove > 0 && _remove != 5) || (_remove < 0 && _remove == -5) || _remove == 0) {
        pool_backward(poolLoss[offset + 4], poolIndex[offset + 4], poolInputLoss);
      }

	  offset = 4 * _poolfunctions;
      //after
      unconcat(poolLoss[offset], poolLoss[offset + 1], poolLoss[offset + 2], poolLoss[offset + 3], poolLoss[offset + 4], afterrepresentLoss);

      //avg pooling
      if ((_remove > 0 && _remove != 1) || (_remove < 0 && _remove == -1) || _remove == 0) {
        pool_backward(poolLoss[offset], poolIndex[offset],  poolInputLoss);
      }
      //max pooling
      if ((_remove > 0 && _remove != 2) || (_remove < 0 && _remove == -2) || _remove == 0) {
        pool_backward(poolLoss[offset + 1], poolIndex[offset + 1], poolInputLoss);
      }
      //min pooling
      if ((_remove > 0 && _remove != 3) || (_remove < 0 && _remove == -3) || _remove == 0) {
        pool_backward(poolLoss[offset + 2], poolIndex[offset + 2], poolInputLoss);
      }
      //std pooling
      if ((_remove > 0 && _remove != 4) || (_remove < 0 && _remove == -4) || _remove == 0) {
        pool_backward(poolLoss[offset + 3], poolIndex[offset + 3], poolInputLoss);
      }
      //pro pooling
      if ((_remove > 0 && _remove != 5) || (_remove < 0 && _remove == -5) || _remove == 0) {
        pool_backward(poolLoss[offset + 4], poolIndex[offset + 4], poolInputLoss);
      }


      sentBegin = 0;
      for(int sentIdx=0;sentIdx<sentNum;sentIdx++) {
    	  int sentLength = sentIdx==0 ? example.sentEnd[sentIdx] : example.sentEnd[sentIdx]-example.sentEnd[sentIdx-1];
    	  int seqIdx = sentBegin;
    	  for(int idx=0;idx<sentLength;idx++, seqIdx++) {
    		  unconcat(projectLoss[seqIdx], v_recursive_hidden_loss[sentIdx][idx], poolInputLoss[seqIdx]);
    	  }

          _recursive.ComputeBackwardLoss(v_recursive_input[sentIdx], example.dep[sentIdx], example.depType[sentIdx],
        		  v_recursive_v_rsc[sentIdx], v_recursive_rsp[sentIdx],
				  v_recursive_hidden[sentIdx], v_recursive_hidden_loss[sentIdx], v_recursive_input_loss[sentIdx]);


          seqIdx = sentBegin;
    	  for(int idx=0;idx<sentLength;idx++, seqIdx++) {
    		  inputLoss[seqIdx] += v_recursive_input_loss[sentIdx][idx];
    	  }

    	  sentBegin += sentLength;
      }



      for (int idx = 0; idx < seq_size; idx++) {
        _tanh_project.ComputeBackwardLoss(rnn_hidden_merge[idx], project[idx], projectLoss[idx], rnn_hidden_mergeLoss[idx]);
      }

      for (int idx = 0; idx < seq_size; idx++) {
        unconcat(rnn_hidden_leftLoss[idx], rnn_hidden_rightLoss[idx], rnn_hidden_mergeLoss[idx]);
      }

      _rnn_left.ComputeBackwardLoss(input, rnn_hidden_left_reset, rnn_hidden_left_afterreset, rnn_hidden_left_update, rnn_hidden_left_current, rnn_hidden_left, rnn_hidden_leftLoss, inputLoss);
      _rnn_right.ComputeBackwardLoss(input, rnn_hidden_right_reset, rnn_hidden_right_afterreset, rnn_hidden_right_update, rnn_hidden_right_current, rnn_hidden_right, rnn_hidden_rightLoss, inputLoss);

	  
      // word context
      windowlized_backward(wordrepresentLoss, inputLoss, _wordcontext);

      for (int idx = 0; idx < seq_size; idx++) {
        wordprimeLoss[idx] += wordrepresentLoss[idx];
      }

      if (_words.bEmbFineTune()) {
        for (int idx = 0; idx < seq_size; idx++) {
          const Feature& feature = example.m_features[idx];
          const vector<int>& words = feature.words;
          wordprimeLoss[idx] = wordprimeLoss[idx] * wordprimeMask[idx];
          _words.EmbLoss(words[0], wordprimeLoss[idx]);
        }
      }

      //release
      FreeSpace(&wordprime);
      FreeSpace(&wordprimeLoss);
      FreeSpace(&wordprimeMask);
      FreeSpace(&wordrepresent);
      FreeSpace(&wordrepresentLoss);

      FreeSpace(&input);
      FreeSpace(&inputLoss);

      FreeSpace(&rnn_hidden_left_reset);
      FreeSpace(&rnn_hidden_left_update);
      FreeSpace(&rnn_hidden_left_afterreset);
      FreeSpace(&rnn_hidden_left_current);
      FreeSpace(&rnn_hidden_left);
      FreeSpace(&rnn_hidden_leftLoss);

      FreeSpace(&rnn_hidden_right_reset);
      FreeSpace(&rnn_hidden_right_update);
      FreeSpace(&rnn_hidden_right_afterreset);
      FreeSpace(&rnn_hidden_right_current);
      FreeSpace(&rnn_hidden_right);
      FreeSpace(&rnn_hidden_rightLoss);

      FreeSpace(&rnn_hidden_merge);
      FreeSpace(&rnn_hidden_mergeLoss);


      for(int sentIdx=0;sentIdx<sentNum;sentIdx++) {
    	  FreeSpace(&(v_recursive_input[sentIdx]));
    	  FreeSpace(&(v_recursive_input_loss[sentIdx]));
    	  FreeSpace(&(v_recursive_rsp[sentIdx]));
          for(int i=0;i<v_recursive_v_rsc[sentIdx].size();i++)
        	  FreeSpace(&(v_recursive_v_rsc[sentIdx][i]));
    	  FreeSpace(&(v_recursive_hidden[sentIdx]));
    	  FreeSpace(&(v_recursive_hidden_loss[sentIdx]));
      }


      FreeSpace(&poolInput);
      FreeSpace(&poolInputLoss);

      FreeSpace(&project);
      FreeSpace(&projectLoss);

      for (int idm = 0; idm < _poolmanners; idm++) {
        FreeSpace(&(pool[idm]));
        FreeSpace(&(poolLoss[idm]));
        FreeSpace(&(poolIndex[idm]));
      }

      FreeSpace(&beforerepresent);
      FreeSpace(&beforerepresentLoss);
      FreeSpace(&formerrepresent);
      FreeSpace(&formerrepresentLoss);
      FreeSpace(&middlerepresent);
      FreeSpace(&middlerepresentLoss);
	  FreeSpace(&latterrepresent);
      FreeSpace(&latterrepresentLoss);
	  FreeSpace(&afterrepresent);
      FreeSpace(&afterrepresentLoss);

      FreeSpace(&poolmerge);
      FreeSpace(&poolmergeLoss);
      FreeSpace(&output);
      FreeSpace(&outputLoss);

    }

    if (_eval.getAccuracy() < 0) {
      std::cout << "strange" << std::endl;
    }

    return cost;
  }

  int predict(const Example& example, vector<dtype>& results) {
		const vector<Feature>& features = example.m_features;
    int seq_size = features.size();
    int offset = 0;
    int sentNum = example.dep.size();

    Tensor<xpu, 3, dtype> input;
    Tensor<xpu, 3, dtype> project;

    Tensor<xpu, 3, dtype> rnn_hidden_left_update;
    Tensor<xpu, 3, dtype> rnn_hidden_left_reset;
    Tensor<xpu, 3, dtype> rnn_hidden_left;
    Tensor<xpu, 3, dtype> rnn_hidden_left_afterreset;
    Tensor<xpu, 3, dtype> rnn_hidden_left_current;

    Tensor<xpu, 3, dtype> rnn_hidden_right_update;
    Tensor<xpu, 3, dtype> rnn_hidden_right_reset;
    Tensor<xpu, 3, dtype> rnn_hidden_right;
    Tensor<xpu, 3, dtype> rnn_hidden_right_afterreset;
    Tensor<xpu, 3, dtype> rnn_hidden_right_current;

    Tensor<xpu, 3, dtype> rnn_hidden_merge;

    vector< Tensor<xpu, 3, dtype> > v_recursive_input(sentNum);
    vector< Tensor<xpu, 3, dtype> > v_recursive_rsp(sentNum);
    vector< vector< Tensor<xpu, 3, dtype> > > v_recursive_v_rsc(sentNum);
    vector< Tensor<xpu, 3, dtype> > v_recursive_hidden(sentNum);

    Tensor<xpu, 3, dtype> poolInput;

    vector<Tensor<xpu, 2, dtype> > pool(_poolmanners);
    vector<Tensor<xpu, 3, dtype> > poolIndex(_poolmanners);

    Tensor<xpu, 2, dtype> poolmerge;
    Tensor<xpu, 2, dtype> output;

    Tensor<xpu, 3, dtype> wordprime, wordrepresent;

	hash_set<int> beforeIndex, formerIndex, middleIndex, latterIndex, afterIndex;
      Tensor<xpu, 2, dtype> beforerepresent;
      Tensor<xpu, 2, dtype> formerrepresent;
	  Tensor<xpu, 2, dtype> middlerepresent;
      Tensor<xpu, 2, dtype> latterrepresent;
	  Tensor<xpu, 2, dtype> afterrepresent;

    static hash_set<int>::iterator it;

    //initialize
    wordprime = NewTensor<xpu>(Shape3(seq_size, 1, _wordDim), 0.0);
    wordrepresent = NewTensor<xpu>(Shape3(seq_size, 1, _token_representation_size), 0.0);

    input = NewTensor<xpu>(Shape3(seq_size, 1, _inputsize), 0.0);

    rnn_hidden_left_reset = NewTensor<xpu>(Shape3(seq_size, 1, _rnnHiddenSize), 0.0);
    rnn_hidden_left_update = NewTensor<xpu>(Shape3(seq_size, 1, _rnnHiddenSize), 0.0);
    rnn_hidden_left_afterreset = NewTensor<xpu>(Shape3(seq_size, 1, _rnnHiddenSize), 0.0);
    rnn_hidden_left_current = NewTensor<xpu>(Shape3(seq_size, 1, _rnnHiddenSize), 0.0);
    rnn_hidden_left = NewTensor<xpu>(Shape3(seq_size, 1, _rnnHiddenSize), 0.0);

    rnn_hidden_right_reset = NewTensor<xpu>(Shape3(seq_size, 1, _rnnHiddenSize), 0.0);
    rnn_hidden_right_update = NewTensor<xpu>(Shape3(seq_size, 1, _rnnHiddenSize), 0.0);
    rnn_hidden_right_afterreset = NewTensor<xpu>(Shape3(seq_size, 1, _rnnHiddenSize), 0.0);
    rnn_hidden_right_current = NewTensor<xpu>(Shape3(seq_size, 1, _rnnHiddenSize), 0.0);
    rnn_hidden_right = NewTensor<xpu>(Shape3(seq_size, 1, _rnnHiddenSize), 0.0);

    rnn_hidden_merge = NewTensor<xpu>(Shape3(seq_size, 1, 2*_rnnHiddenSize), 0.0);

    project = NewTensor<xpu>(Shape3(seq_size, 1, _hiddensize), 0.0);


    for(int sentIdx=0;sentIdx<sentNum;sentIdx++) {
  	  int sentLength = sentIdx==0 ? example.sentEnd[sentIdx] : example.sentEnd[sentIdx]-example.sentEnd[sentIdx-1];
  	  v_recursive_input[sentIdx] = NewTensor<xpu>(Shape3(sentLength, 1, _inputsize), 0.0);
  	  v_recursive_rsp[sentIdx] = NewTensor<xpu>(Shape3(sentLength, 1, _hiddensize), 0.0);
  	  v_recursive_hidden[sentIdx] = NewTensor<xpu>(Shape3(sentLength, 1, _hiddensize), 0.0);
    }

    poolInput = NewTensor<xpu>(Shape3(seq_size, 1, _poolInputSize), 0.0);

    for (int idm = 0; idm < _poolmanners; idm++) {
      pool[idm] = NewTensor<xpu>(Shape2(1, _poolInputSize), 0.0);
      poolIndex[idm] = NewTensor<xpu>(Shape3(seq_size, 1, _poolInputSize), 0.0);
    }

    beforerepresent = NewTensor<xpu>(Shape2(1, _poolfunctions * _poolInputSize), 0.0);
    formerrepresent = NewTensor<xpu>(Shape2(1, _poolfunctions * _poolInputSize), 0.0);
    middlerepresent = NewTensor<xpu>(Shape2(1, _poolfunctions * _poolInputSize), 0.0);
	  latterrepresent = NewTensor<xpu>(Shape2(1, _poolfunctions * _poolInputSize), 0.0);
	  afterrepresent = NewTensor<xpu>(Shape2(1, _poolfunctions * _poolInputSize), 0.0);

    poolmerge = NewTensor<xpu>(Shape2(1, _poolsize), 0.0);
    output = NewTensor<xpu>(Shape2(1, _labelSize), 0.0);

    //forward propagation
    //input setting, and linear setting
    for (int idx = 0; idx < seq_size; idx++) {
      const Feature& feature = features[idx];
      //linear features should not be dropped out

      const vector<int>& words = feature.words;
	    if (idx < example.formerTkBegin) {
        beforeIndex.insert(idx);
      } else if(idx >= example.formerTkBegin && idx <= example.formerTkEnd) {
			formerIndex.insert(idx);
		} else if (idx >= example.latterTkBegin && idx <= example.latterTkEnd) {
        latterIndex.insert(idx);
      } else if (idx > example.latterTkEnd) {
        afterIndex.insert(idx);
      } else {
        middleIndex.insert(idx);
      }

      _words.GetEmb(words[0], wordprime[idx]);

    }

    for (int idx = 0; idx < seq_size; idx++) {
      wordrepresent[idx] += wordprime[idx];
    }

    windowlized(wordrepresent, input, _wordcontext);

    _rnn_left.ComputeForwardScore(input, rnn_hidden_left_reset, rnn_hidden_left_afterreset, rnn_hidden_left_update, rnn_hidden_left_current, rnn_hidden_left);
    _rnn_right.ComputeForwardScore(input, rnn_hidden_right_reset, rnn_hidden_right_afterreset, rnn_hidden_right_update, rnn_hidden_right_current, rnn_hidden_right);


    for (int idx = 0; idx < seq_size; idx++) {
        concat(rnn_hidden_left[idx], rnn_hidden_right[idx], rnn_hidden_merge[idx]);
    }

    // do we need a convolution? future work, currently needn't
    for (int idx = 0; idx < seq_size; idx++) {
      _tanh_project.ComputeForwardScore(rnn_hidden_merge[idx], project[idx]);
    }


    int sentBegin = 0;
    for(int sentIdx=0;sentIdx<sentNum;sentIdx++) {
  	  int sentLength = sentIdx==0 ? example.sentEnd[sentIdx] : example.sentEnd[sentIdx]-example.sentEnd[sentIdx-1];
  	  int seqIdx = sentBegin;
  	  for(int idx=0;idx<sentLength;idx++, seqIdx++) {
  		  v_recursive_input[sentIdx][idx] += input[seqIdx];
  	  }

        _recursive.ComputeForwardScore(v_recursive_input[sentIdx], example.dep[sentIdx], example.depType[sentIdx],
      		  v_recursive_v_rsc[sentIdx], v_recursive_rsp[sentIdx],
      		  v_recursive_hidden[sentIdx]);

        seqIdx = sentBegin;
  	  for(int idx=0;idx<sentLength;idx++, seqIdx++) {
  		  concat(project[seqIdx], v_recursive_hidden[sentIdx][idx], poolInput[seqIdx]);
  	  }

  	  sentBegin += sentLength;
    }


    offset = 0;
    //before
    //avg pooling
    if ((_remove > 0 && _remove != 1) || (_remove < 0 && _remove == -1) || _remove == 0) {
      avgpool_forward(poolInput, pool[offset], poolIndex[offset], beforeIndex);
    }
    //max pooling
    if ((_remove > 0 && _remove != 2) || (_remove < 0 && _remove == -2) || _remove == 0) {
      maxpool_forward(poolInput, pool[offset + 1], poolIndex[offset + 1], beforeIndex);
    }
    //min pooling
    if ((_remove > 0 && _remove != 3) || (_remove < 0 && _remove == -3) || _remove == 0) {
      minpool_forward(poolInput, pool[offset + 2], poolIndex[offset + 2], beforeIndex);
    }
    //std pooling
    if ((_remove > 0 && _remove != 4) || (_remove < 0 && _remove == -4) || _remove == 0) {
      stdpool_forward(poolInput, pool[offset + 3], poolIndex[offset + 3], beforeIndex);
    }
    //pro pooling
    if ((_remove > 0 && _remove != 5) || (_remove < 0 && _remove == -5) || _remove == 0) {
      propool_forward(poolInput, pool[offset + 4], poolIndex[offset + 4], beforeIndex);
    }

    concat(pool[offset], pool[offset + 1], pool[offset + 2], pool[offset + 3], pool[offset + 4], beforerepresent);

    offset = _poolfunctions;
    //former
    //avg pooling
    if ((_remove > 0 && _remove != 1) || (_remove < 0 && _remove == -1) || _remove == 0) {
      avgpool_forward(poolInput, pool[offset], poolIndex[offset], formerIndex);
    }
    //max pooling
    if ((_remove > 0 && _remove != 2) || (_remove < 0 && _remove == -2) || _remove == 0) {
      maxpool_forward(poolInput, pool[offset + 1], poolIndex[offset + 1], formerIndex);
    }
    //min pooling
    if ((_remove > 0 && _remove != 3) || (_remove < 0 && _remove == -3) || _remove == 0) {
      minpool_forward(poolInput, pool[offset + 2], poolIndex[offset + 2], formerIndex);
    }
    //std pooling
    if ((_remove > 0 && _remove != 4) || (_remove < 0 && _remove == -4) || _remove == 0) {
      stdpool_forward(poolInput, pool[offset + 3], poolIndex[offset + 3], formerIndex);
    }
    //pro pooling
    if ((_remove > 0 && _remove != 5) || (_remove < 0 && _remove == -5) || _remove == 0) {
      propool_forward(poolInput, pool[offset + 4], poolIndex[offset + 4], formerIndex);
    }

    concat(pool[offset], pool[offset + 1], pool[offset + 2], pool[offset + 3], pool[offset + 4], formerrepresent);

    offset = 2 * _poolfunctions;
    //middle
    //avg pooling
    if ((_remove > 0 && _remove != 1) || (_remove < 0 && _remove == -1) || _remove == 0) {
      avgpool_forward(poolInput, pool[offset], poolIndex[offset], middleIndex);
    }
    //max pooling
    if ((_remove > 0 && _remove != 2) || (_remove < 0 && _remove == -2) || _remove == 0) {
      maxpool_forward(poolInput, pool[offset + 1], poolIndex[offset + 1], middleIndex);
    }
    //min pooling
    if ((_remove > 0 && _remove != 3) || (_remove < 0 && _remove == -3) || _remove == 0) {
      minpool_forward(poolInput, pool[offset + 2], poolIndex[offset + 2], middleIndex);
    }
    //std pooling
    if ((_remove > 0 && _remove != 4) || (_remove < 0 && _remove == -4) || _remove == 0) {
      stdpool_forward(poolInput, pool[offset + 3], poolIndex[offset + 3], middleIndex);
    }
    //pro pooling
    if ((_remove > 0 && _remove != 5) || (_remove < 0 && _remove == -5) || _remove == 0) {
      propool_forward(poolInput, pool[offset + 4], poolIndex[offset + 4], middleIndex);
    }

    concat(pool[offset], pool[offset + 1], pool[offset + 2], pool[offset + 3], pool[offset + 4], middlerepresent);

	  offset = 3 * _poolfunctions;
    //latter
    //avg pooling
    if ((_remove > 0 && _remove != 1) || (_remove < 0 && _remove == -1) || _remove == 0) {
      avgpool_forward(poolInput, pool[offset], poolIndex[offset], latterIndex);
    }
    //max pooling
    if ((_remove > 0 && _remove != 2) || (_remove < 0 && _remove == -2) || _remove == 0) {
      maxpool_forward(poolInput, pool[offset + 1], poolIndex[offset + 1], latterIndex);
    }
    //min pooling
    if ((_remove > 0 && _remove != 3) || (_remove < 0 && _remove == -3) || _remove == 0) {
      minpool_forward(poolInput, pool[offset + 2], poolIndex[offset + 2], latterIndex);
    }
    //std pooling
    if ((_remove > 0 && _remove != 4) || (_remove < 0 && _remove == -4) || _remove == 0) {
      stdpool_forward(poolInput, pool[offset + 3], poolIndex[offset + 3], latterIndex);
    }
    //pro pooling
    if ((_remove > 0 && _remove != 5) || (_remove < 0 && _remove == -5) || _remove == 0) {
      propool_forward(poolInput, pool[offset + 4], poolIndex[offset + 4], latterIndex);
    }

    concat(pool[offset], pool[offset + 1], pool[offset + 2], pool[offset + 3], pool[offset + 4], latterrepresent);

	  offset = 4 * _poolfunctions;
    //after
    //avg pooling
    if ((_remove > 0 && _remove != 1) || (_remove < 0 && _remove == -1) || _remove == 0) {
      avgpool_forward(poolInput, pool[offset], poolIndex[offset], afterIndex);
    }
    //max pooling
    if ((_remove > 0 && _remove != 2) || (_remove < 0 && _remove == -2) || _remove == 0) {
      maxpool_forward(poolInput, pool[offset + 1], poolIndex[offset + 1], afterIndex);
    }
    //min pooling
    if ((_remove > 0 && _remove != 3) || (_remove < 0 && _remove == -3) || _remove == 0) {
      minpool_forward(poolInput, pool[offset + 2], poolIndex[offset + 2], afterIndex);
    }
    //std pooling
    if ((_remove > 0 && _remove != 4) || (_remove < 0 && _remove == -4) || _remove == 0) {
      stdpool_forward(poolInput, pool[offset + 3], poolIndex[offset + 3], afterIndex);
    }
    //pro pooling
    if ((_remove > 0 && _remove != 5) || (_remove < 0 && _remove == -5) || _remove == 0) {
      propool_forward(poolInput, pool[offset + 4], poolIndex[offset + 4], afterIndex);
    }

    concat(pool[offset], pool[offset + 1], pool[offset + 2], pool[offset + 3], pool[offset + 4], afterrepresent);

  concat(beforerepresent, formerrepresent, middlerepresent, latterrepresent, afterrepresent, poolmerge);

    _olayer_linear.ComputeForwardScore(poolmerge, output);

    // decode algorithm
    int optLabel = softmax_predict(output, results);

    //release
    FreeSpace(&wordprime);
    FreeSpace(&wordrepresent);

    FreeSpace(&input);

    FreeSpace(&rnn_hidden_left_reset);
    FreeSpace(&rnn_hidden_left_update);
    FreeSpace(&rnn_hidden_left_afterreset);
    FreeSpace(&rnn_hidden_left_current);
    FreeSpace(&rnn_hidden_left);

    FreeSpace(&rnn_hidden_right_reset);
    FreeSpace(&rnn_hidden_right_update);
    FreeSpace(&rnn_hidden_right_afterreset);
    FreeSpace(&rnn_hidden_right_current);
    FreeSpace(&rnn_hidden_right);

    FreeSpace(&rnn_hidden_merge);

    FreeSpace(&project);

    for(int sentIdx=0;sentIdx<sentNum;sentIdx++) {
  	  FreeSpace(&(v_recursive_input[sentIdx]));
  	  FreeSpace(&(v_recursive_rsp[sentIdx]));
        for(int i=0;i<v_recursive_v_rsc[sentIdx].size();i++)
      	  FreeSpace(&(v_recursive_v_rsc[sentIdx][i]));
  	  FreeSpace(&(v_recursive_hidden[sentIdx]));
    }

    FreeSpace(&poolInput);


    for (int idm = 0; idm < _poolmanners; idm++) {
      FreeSpace(&(pool[idm]));
      FreeSpace(&(poolIndex[idm]));
    }

    FreeSpace(&beforerepresent);
    FreeSpace(&formerrepresent);
    FreeSpace(&middlerepresent);
	  FreeSpace(&latterrepresent);
	  FreeSpace(&afterrepresent);

    FreeSpace(&poolmerge);
    FreeSpace(&output);

    return optLabel;
  }

  dtype computeScore(const Example& example) {
		const vector<Feature>& features = example.m_features;
  int seq_size = features.size();
  int offset = 0;
  int sentNum = example.dep.size();

  Tensor<xpu, 3, dtype> input;
  Tensor<xpu, 3, dtype> project;

  Tensor<xpu, 3, dtype> rnn_hidden_left_update;
  Tensor<xpu, 3, dtype> rnn_hidden_left_reset;
  Tensor<xpu, 3, dtype> rnn_hidden_left;
  Tensor<xpu, 3, dtype> rnn_hidden_left_afterreset;
  Tensor<xpu, 3, dtype> rnn_hidden_left_current;

  Tensor<xpu, 3, dtype> rnn_hidden_right_update;
  Tensor<xpu, 3, dtype> rnn_hidden_right_reset;
  Tensor<xpu, 3, dtype> rnn_hidden_right;
  Tensor<xpu, 3, dtype> rnn_hidden_right_afterreset;
  Tensor<xpu, 3, dtype> rnn_hidden_right_current;

  Tensor<xpu, 3, dtype> rnn_hidden_merge;

  vector< Tensor<xpu, 3, dtype> > v_recursive_input(sentNum);
  vector< Tensor<xpu, 3, dtype> > v_recursive_rsp(sentNum);
  vector< vector< Tensor<xpu, 3, dtype> > > v_recursive_v_rsc(sentNum);
  vector< Tensor<xpu, 3, dtype> > v_recursive_hidden(sentNum);

  Tensor<xpu, 3, dtype> poolInput;

  vector<Tensor<xpu, 2, dtype> > pool(_poolmanners);
  vector<Tensor<xpu, 3, dtype> > poolIndex(_poolmanners);

  Tensor<xpu, 2, dtype> poolmerge;
  Tensor<xpu, 2, dtype> output;

  Tensor<xpu, 3, dtype> wordprime, wordrepresent;

	hash_set<int> beforeIndex, formerIndex, middleIndex, latterIndex, afterIndex;
    Tensor<xpu, 2, dtype> beforerepresent;
    Tensor<xpu, 2, dtype> formerrepresent;
	  Tensor<xpu, 2, dtype> middlerepresent;
    Tensor<xpu, 2, dtype> latterrepresent;
	  Tensor<xpu, 2, dtype> afterrepresent;

  static hash_set<int>::iterator it;

  //initialize
  wordprime = NewTensor<xpu>(Shape3(seq_size, 1, _wordDim), 0.0);
  wordrepresent = NewTensor<xpu>(Shape3(seq_size, 1, _token_representation_size), 0.0);

  input = NewTensor<xpu>(Shape3(seq_size, 1, _inputsize), 0.0);

  rnn_hidden_left_reset = NewTensor<xpu>(Shape3(seq_size, 1, _rnnHiddenSize), 0.0);
  rnn_hidden_left_update = NewTensor<xpu>(Shape3(seq_size, 1, _rnnHiddenSize), 0.0);
  rnn_hidden_left_afterreset = NewTensor<xpu>(Shape3(seq_size, 1, _rnnHiddenSize), 0.0);
  rnn_hidden_left_current = NewTensor<xpu>(Shape3(seq_size, 1, _rnnHiddenSize), 0.0);
  rnn_hidden_left = NewTensor<xpu>(Shape3(seq_size, 1, _rnnHiddenSize), 0.0);

  rnn_hidden_right_reset = NewTensor<xpu>(Shape3(seq_size, 1, _rnnHiddenSize), 0.0);
  rnn_hidden_right_update = NewTensor<xpu>(Shape3(seq_size, 1, _rnnHiddenSize), 0.0);
  rnn_hidden_right_afterreset = NewTensor<xpu>(Shape3(seq_size, 1, _rnnHiddenSize), 0.0);
  rnn_hidden_right_current = NewTensor<xpu>(Shape3(seq_size, 1, _rnnHiddenSize), 0.0);
  rnn_hidden_right = NewTensor<xpu>(Shape3(seq_size, 1, _rnnHiddenSize), 0.0);

  rnn_hidden_merge = NewTensor<xpu>(Shape3(seq_size, 1, 2*_rnnHiddenSize), 0.0);

  project = NewTensor<xpu>(Shape3(seq_size, 1, _hiddensize), 0.0);


  for(int sentIdx=0;sentIdx<sentNum;sentIdx++) {
	  int sentLength = sentIdx==0 ? example.sentEnd[sentIdx] : example.sentEnd[sentIdx]-example.sentEnd[sentIdx-1];
	  v_recursive_input[sentIdx] = NewTensor<xpu>(Shape3(sentLength, 1, _inputsize), 0.0);
	  v_recursive_rsp[sentIdx] = NewTensor<xpu>(Shape3(sentLength, 1, _hiddensize), 0.0);
	  v_recursive_hidden[sentIdx] = NewTensor<xpu>(Shape3(sentLength, 1, _hiddensize), 0.0);
  }

  poolInput = NewTensor<xpu>(Shape3(seq_size, 1, _poolInputSize), 0.0);

  for (int idm = 0; idm < _poolmanners; idm++) {
    pool[idm] = NewTensor<xpu>(Shape2(1, _poolInputSize), 0.0);
    poolIndex[idm] = NewTensor<xpu>(Shape3(seq_size, 1, _poolInputSize), 0.0);
  }

  beforerepresent = NewTensor<xpu>(Shape2(1, _poolfunctions * _poolInputSize), 0.0);
  formerrepresent = NewTensor<xpu>(Shape2(1, _poolfunctions * _poolInputSize), 0.0);
  middlerepresent = NewTensor<xpu>(Shape2(1, _poolfunctions * _poolInputSize), 0.0);
	  latterrepresent = NewTensor<xpu>(Shape2(1, _poolfunctions * _poolInputSize), 0.0);
	  afterrepresent = NewTensor<xpu>(Shape2(1, _poolfunctions * _poolInputSize), 0.0);

  poolmerge = NewTensor<xpu>(Shape2(1, _poolsize), 0.0);
  output = NewTensor<xpu>(Shape2(1, _labelSize), 0.0);

  //forward propagation
  //input setting, and linear setting
  for (int idx = 0; idx < seq_size; idx++) {
    const Feature& feature = features[idx];
    //linear features should not be dropped out

    const vector<int>& words = feature.words;
	    if (idx < example.formerTkBegin) {
      beforeIndex.insert(idx);
    } else if(idx >= example.formerTkBegin && idx <= example.formerTkEnd) {
			formerIndex.insert(idx);
		} else if (idx >= example.latterTkBegin && idx <= example.latterTkEnd) {
      latterIndex.insert(idx);
    } else if (idx > example.latterTkEnd) {
      afterIndex.insert(idx);
    } else {
      middleIndex.insert(idx);
    }

    _words.GetEmb(words[0], wordprime[idx]);

  }

  for (int idx = 0; idx < seq_size; idx++) {
    wordrepresent[idx] += wordprime[idx];
  }

  windowlized(wordrepresent, input, _wordcontext);

  _rnn_left.ComputeForwardScore(input, rnn_hidden_left_reset, rnn_hidden_left_afterreset, rnn_hidden_left_update, rnn_hidden_left_current, rnn_hidden_left);
  _rnn_right.ComputeForwardScore(input, rnn_hidden_right_reset, rnn_hidden_right_afterreset, rnn_hidden_right_update, rnn_hidden_right_current, rnn_hidden_right);


  for (int idx = 0; idx < seq_size; idx++) {
      concat(rnn_hidden_left[idx], rnn_hidden_right[idx], rnn_hidden_merge[idx]);
  }

  // do we need a convolution? future work, currently needn't
  for (int idx = 0; idx < seq_size; idx++) {
    _tanh_project.ComputeForwardScore(rnn_hidden_merge[idx], project[idx]);
  }


  int sentBegin = 0;
  for(int sentIdx=0;sentIdx<sentNum;sentIdx++) {
	  int sentLength = sentIdx==0 ? example.sentEnd[sentIdx] : example.sentEnd[sentIdx]-example.sentEnd[sentIdx-1];
	  int seqIdx = sentBegin;
	  for(int idx=0;idx<sentLength;idx++, seqIdx++) {
		  v_recursive_input[sentIdx][idx] += input[seqIdx];
	  }

      _recursive.ComputeForwardScore(v_recursive_input[sentIdx], example.dep[sentIdx], example.depType[sentIdx],
    		  v_recursive_v_rsc[sentIdx], v_recursive_rsp[sentIdx],
    		  v_recursive_hidden[sentIdx]);

      seqIdx = sentBegin;
	  for(int idx=0;idx<sentLength;idx++, seqIdx++) {
		  concat(project[seqIdx], v_recursive_hidden[sentIdx][idx], poolInput[seqIdx]);
	  }

	  sentBegin += sentLength;
  }


  offset = 0;
  //before
  //avg pooling
  if ((_remove > 0 && _remove != 1) || (_remove < 0 && _remove == -1) || _remove == 0) {
    avgpool_forward(poolInput, pool[offset], poolIndex[offset], beforeIndex);
  }
  //max pooling
  if ((_remove > 0 && _remove != 2) || (_remove < 0 && _remove == -2) || _remove == 0) {
    maxpool_forward(poolInput, pool[offset + 1], poolIndex[offset + 1], beforeIndex);
  }
  //min pooling
  if ((_remove > 0 && _remove != 3) || (_remove < 0 && _remove == -3) || _remove == 0) {
    minpool_forward(poolInput, pool[offset + 2], poolIndex[offset + 2], beforeIndex);
  }
  //std pooling
  if ((_remove > 0 && _remove != 4) || (_remove < 0 && _remove == -4) || _remove == 0) {
    stdpool_forward(poolInput, pool[offset + 3], poolIndex[offset + 3], beforeIndex);
  }
  //pro pooling
  if ((_remove > 0 && _remove != 5) || (_remove < 0 && _remove == -5) || _remove == 0) {
    propool_forward(poolInput, pool[offset + 4], poolIndex[offset + 4], beforeIndex);
  }

  concat(pool[offset], pool[offset + 1], pool[offset + 2], pool[offset + 3], pool[offset + 4], beforerepresent);

  offset = _poolfunctions;
  //former
  //avg pooling
  if ((_remove > 0 && _remove != 1) || (_remove < 0 && _remove == -1) || _remove == 0) {
    avgpool_forward(poolInput, pool[offset], poolIndex[offset], formerIndex);
  }
  //max pooling
  if ((_remove > 0 && _remove != 2) || (_remove < 0 && _remove == -2) || _remove == 0) {
    maxpool_forward(poolInput, pool[offset + 1], poolIndex[offset + 1], formerIndex);
  }
  //min pooling
  if ((_remove > 0 && _remove != 3) || (_remove < 0 && _remove == -3) || _remove == 0) {
    minpool_forward(poolInput, pool[offset + 2], poolIndex[offset + 2], formerIndex);
  }
  //std pooling
  if ((_remove > 0 && _remove != 4) || (_remove < 0 && _remove == -4) || _remove == 0) {
    stdpool_forward(poolInput, pool[offset + 3], poolIndex[offset + 3], formerIndex);
  }
  //pro pooling
  if ((_remove > 0 && _remove != 5) || (_remove < 0 && _remove == -5) || _remove == 0) {
    propool_forward(poolInput, pool[offset + 4], poolIndex[offset + 4], formerIndex);
  }

  concat(pool[offset], pool[offset + 1], pool[offset + 2], pool[offset + 3], pool[offset + 4], formerrepresent);

  offset = 2 * _poolfunctions;
  //middle
  //avg pooling
  if ((_remove > 0 && _remove != 1) || (_remove < 0 && _remove == -1) || _remove == 0) {
    avgpool_forward(poolInput, pool[offset], poolIndex[offset], middleIndex);
  }
  //max pooling
  if ((_remove > 0 && _remove != 2) || (_remove < 0 && _remove == -2) || _remove == 0) {
    maxpool_forward(poolInput, pool[offset + 1], poolIndex[offset + 1], middleIndex);
  }
  //min pooling
  if ((_remove > 0 && _remove != 3) || (_remove < 0 && _remove == -3) || _remove == 0) {
    minpool_forward(poolInput, pool[offset + 2], poolIndex[offset + 2], middleIndex);
  }
  //std pooling
  if ((_remove > 0 && _remove != 4) || (_remove < 0 && _remove == -4) || _remove == 0) {
    stdpool_forward(poolInput, pool[offset + 3], poolIndex[offset + 3], middleIndex);
  }
  //pro pooling
  if ((_remove > 0 && _remove != 5) || (_remove < 0 && _remove == -5) || _remove == 0) {
    propool_forward(poolInput, pool[offset + 4], poolIndex[offset + 4], middleIndex);
  }

  concat(pool[offset], pool[offset + 1], pool[offset + 2], pool[offset + 3], pool[offset + 4], middlerepresent);

	  offset = 3 * _poolfunctions;
  //latter
  //avg pooling
  if ((_remove > 0 && _remove != 1) || (_remove < 0 && _remove == -1) || _remove == 0) {
    avgpool_forward(poolInput, pool[offset], poolIndex[offset], latterIndex);
  }
  //max pooling
  if ((_remove > 0 && _remove != 2) || (_remove < 0 && _remove == -2) || _remove == 0) {
    maxpool_forward(poolInput, pool[offset + 1], poolIndex[offset + 1], latterIndex);
  }
  //min pooling
  if ((_remove > 0 && _remove != 3) || (_remove < 0 && _remove == -3) || _remove == 0) {
    minpool_forward(poolInput, pool[offset + 2], poolIndex[offset + 2], latterIndex);
  }
  //std pooling
  if ((_remove > 0 && _remove != 4) || (_remove < 0 && _remove == -4) || _remove == 0) {
    stdpool_forward(poolInput, pool[offset + 3], poolIndex[offset + 3], latterIndex);
  }
  //pro pooling
  if ((_remove > 0 && _remove != 5) || (_remove < 0 && _remove == -5) || _remove == 0) {
    propool_forward(poolInput, pool[offset + 4], poolIndex[offset + 4], latterIndex);
  }

  concat(pool[offset], pool[offset + 1], pool[offset + 2], pool[offset + 3], pool[offset + 4], latterrepresent);

	  offset = 4 * _poolfunctions;
  //after
  //avg pooling
  if ((_remove > 0 && _remove != 1) || (_remove < 0 && _remove == -1) || _remove == 0) {
    avgpool_forward(poolInput, pool[offset], poolIndex[offset], afterIndex);
  }
  //max pooling
  if ((_remove > 0 && _remove != 2) || (_remove < 0 && _remove == -2) || _remove == 0) {
    maxpool_forward(poolInput, pool[offset + 1], poolIndex[offset + 1], afterIndex);
  }
  //min pooling
  if ((_remove > 0 && _remove != 3) || (_remove < 0 && _remove == -3) || _remove == 0) {
    minpool_forward(poolInput, pool[offset + 2], poolIndex[offset + 2], afterIndex);
  }
  //std pooling
  if ((_remove > 0 && _remove != 4) || (_remove < 0 && _remove == -4) || _remove == 0) {
    stdpool_forward(poolInput, pool[offset + 3], poolIndex[offset + 3], afterIndex);
  }
  //pro pooling
  if ((_remove > 0 && _remove != 5) || (_remove < 0 && _remove == -5) || _remove == 0) {
    propool_forward(poolInput, pool[offset + 4], poolIndex[offset + 4], afterIndex);
  }

  concat(pool[offset], pool[offset + 1], pool[offset + 2], pool[offset + 3], pool[offset + 4], afterrepresent);

concat(beforerepresent, formerrepresent, middlerepresent, latterrepresent, afterrepresent, poolmerge);

  _olayer_linear.ComputeForwardScore(poolmerge, output);

  // decode algorithm
  dtype cost = softmax_cost(output, example.m_labels);

  //release
  FreeSpace(&wordprime);
  FreeSpace(&wordrepresent);

  FreeSpace(&input);

  FreeSpace(&rnn_hidden_left_reset);
  FreeSpace(&rnn_hidden_left_update);
  FreeSpace(&rnn_hidden_left_afterreset);
  FreeSpace(&rnn_hidden_left_current);
  FreeSpace(&rnn_hidden_left);

  FreeSpace(&rnn_hidden_right_reset);
  FreeSpace(&rnn_hidden_right_update);
  FreeSpace(&rnn_hidden_right_afterreset);
  FreeSpace(&rnn_hidden_right_current);
  FreeSpace(&rnn_hidden_right);

  FreeSpace(&rnn_hidden_merge);

  FreeSpace(&project);

  for(int sentIdx=0;sentIdx<sentNum;sentIdx++) {
	  FreeSpace(&(v_recursive_input[sentIdx]));
	  FreeSpace(&(v_recursive_rsp[sentIdx]));
      for(int i=0;i<v_recursive_v_rsc[sentIdx].size();i++)
    	  FreeSpace(&(v_recursive_v_rsc[sentIdx][i]));
	  FreeSpace(&(v_recursive_hidden[sentIdx]));
  }

  FreeSpace(&poolInput);


  for (int idm = 0; idm < _poolmanners; idm++) {
    FreeSpace(&(pool[idm]));
    FreeSpace(&(poolIndex[idm]));
  }

  FreeSpace(&beforerepresent);
  FreeSpace(&formerrepresent);
  FreeSpace(&middlerepresent);
	  FreeSpace(&latterrepresent);
	  FreeSpace(&afterrepresent);

  FreeSpace(&poolmerge);
  FreeSpace(&output);

  return cost;
}

  void updateParams(dtype nnRegular, dtype adaAlpha, dtype adaEps) {
    _tanh_project.updateAdaGrad(nnRegular, adaAlpha, adaEps);
    _olayer_linear.updateAdaGrad(nnRegular, adaAlpha, adaEps);

    _rnn_left.updateAdaGrad(nnRegular, adaAlpha, adaEps);
    _rnn_right.updateAdaGrad(nnRegular, adaAlpha, adaEps);
    _recursive.updateAdaGrad(nnRegular, adaAlpha, adaEps);

    _words.updateAdaGrad(nnRegular, adaAlpha, adaEps);
    _pos.updateAdaGrad(nnRegular, adaAlpha, adaEps);
    _ner.updateAdaGrad(nnRegular, adaAlpha, adaEps);
    _sst.updateAdaGrad(nnRegular, adaAlpha, adaEps);
  }

  void writeModel();

  void loadModel();

  void checkgrad(const vector<Example>& examples, Tensor<xpu, 2, dtype> Wd, Tensor<xpu, 2, dtype> gradWd, const string& mark, int iter) {
    int charseed = mark.length();
    for (int i = 0; i < mark.length(); i++) {
      charseed = (int) (mark[i]) * 5 + charseed;
    }
    srand(iter + charseed);
    std::vector<int> idRows, idCols;
    idRows.clear();
    idCols.clear();
    for (int i = 0; i < Wd.size(0); ++i)
      idRows.push_back(i);
    for (int idx = 0; idx < Wd.size(1); idx++)
      idCols.push_back(idx);

    random_shuffle(idRows.begin(), idRows.end());
    random_shuffle(idCols.begin(), idCols.end());

    int check_i = idRows[0], check_j = idCols[0];

    dtype orginValue = Wd[check_i][check_j];

    Wd[check_i][check_j] = orginValue + 0.001;
    dtype lossAdd = 0.0;
    for (int i = 0; i < examples.size(); i++) {
      Example oneExam = examples[i];
      lossAdd += computeScore(oneExam);
    }

    Wd[check_i][check_j] = orginValue - 0.001;
    dtype lossPlus = 0.0;
    for (int i = 0; i < examples.size(); i++) {
      Example oneExam = examples[i];
      lossPlus += computeScore(oneExam);
    }

    dtype mockGrad = (lossAdd - lossPlus) / 0.002;
    mockGrad = mockGrad / examples.size();
    dtype computeGrad = gradWd[check_i][check_j];

    printf("Iteration %d, Checking gradient for %s[%d][%d]:\t", iter, mark.c_str(), check_i, check_j);
    printf("mock grad = %.18f, computed grad = %.18f\n", mockGrad, computeGrad);

    Wd[check_i][check_j] = orginValue;
  }

  void checkgrad(const vector<Example>& examples, Tensor<xpu, 3, dtype> Wd, Tensor<xpu, 3, dtype> gradWd, const string& mark, int iter) {
    int charseed = mark.length();
    for (int i = 0; i < mark.length(); i++) {
      charseed = (int) (mark[i]) * 5 + charseed;
    }
    srand(iter + charseed);
    std::vector<int> idRows, idCols, idThirds;
    idRows.clear();
    idCols.clear();
    idThirds.clear();
    for (int i = 0; i < Wd.size(0); ++i)
      idRows.push_back(i);
    for (int i = 0; i < Wd.size(1); i++)
      idCols.push_back(i);
    for (int i = 0; i < Wd.size(2); i++)
      idThirds.push_back(i);

    random_shuffle(idRows.begin(), idRows.end());
    random_shuffle(idCols.begin(), idCols.end());
    random_shuffle(idThirds.begin(), idThirds.end());

    int check_i = idRows[0], check_j = idCols[0], check_k = idThirds[0];

    dtype orginValue = Wd[check_i][check_j][check_k];

    Wd[check_i][check_j][check_k] = orginValue + 0.001;
    dtype lossAdd = 0.0;
    for (int i = 0; i < examples.size(); i++) {
      Example oneExam = examples[i];
      lossAdd += computeScore(oneExam);
    }

    Wd[check_i][check_j][check_k] = orginValue - 0.001;
    dtype lossPlus = 0.0;
    for (int i = 0; i < examples.size(); i++) {
      Example oneExam = examples[i];
      lossPlus += computeScore(oneExam);
    }

    dtype mockGrad = (lossAdd - lossPlus) / 0.002;
    mockGrad = mockGrad / examples.size();
    dtype computeGrad = gradWd[check_i][check_j][check_k];

    printf("Iteration %d, Checking gradient for %s[%d][%d][%d]:\t", iter, mark.c_str(), check_i, check_j, check_k);
    printf("mock grad = %.18f, computed grad = %.18f\n", mockGrad, computeGrad);

    Wd[check_i][check_j][check_k] = orginValue;
  }

  void checkgrad(const vector<Example>& examples, Tensor<xpu, 2, dtype> Wd, Tensor<xpu, 2, dtype> gradWd, const string& mark, int iter,
      const hash_set<int>& indexes, bool bRow = true) {
    if(indexes.size() == 0) return;
    int charseed = mark.length();
    for (int i = 0; i < mark.length(); i++) {
      charseed = (int) (mark[i]) * 5 + charseed;
    }
    srand(iter + charseed);
    std::vector<int> idRows, idCols;
    idRows.clear();
    idCols.clear();
    static hash_set<int>::iterator it;
    if (bRow) {
      for (it = indexes.begin(); it != indexes.end(); ++it)
        idRows.push_back(*it);
      for (int idx = 0; idx < Wd.size(1); idx++)
        idCols.push_back(idx);
    } else {
      for (it = indexes.begin(); it != indexes.end(); ++it)
        idCols.push_back(*it);
      for (int idx = 0; idx < Wd.size(0); idx++)
        idRows.push_back(idx);
    }

    random_shuffle(idRows.begin(), idRows.end());
    random_shuffle(idCols.begin(), idCols.end());

    int check_i = idRows[0], check_j = idCols[0];

    dtype orginValue = Wd[check_i][check_j];

    Wd[check_i][check_j] = orginValue + 0.001;
    dtype lossAdd = 0.0;
    for (int i = 0; i < examples.size(); i++) {
      Example oneExam = examples[i];
      lossAdd += computeScore(oneExam);
    }

    Wd[check_i][check_j] = orginValue - 0.001;
    dtype lossPlus = 0.0;
    for (int i = 0; i < examples.size(); i++) {
      Example oneExam = examples[i];
      lossPlus += computeScore(oneExam);
    }

    dtype mockGrad = (lossAdd - lossPlus) / 0.002;
    mockGrad = mockGrad / examples.size();
    dtype computeGrad = gradWd[check_i][check_j];

    printf("Iteration %d, Checking gradient for %s[%d][%d]:\t", iter, mark.c_str(), check_i, check_j);
    printf("mock grad = %.18f, computed grad = %.18f\n", mockGrad, computeGrad);

    Wd[check_i][check_j] = orginValue;

  }

  void checkgrads(const vector<Example>& examples, int iter) {

    checkgrad(examples, _olayer_linear._W, _olayer_linear._gradW, "_olayer_linear._W", iter);
    checkgrad(examples, _olayer_linear._b, _olayer_linear._gradb, "_olayer_linear._b", iter);

    checkgrad(examples, _tanh_project._W, _tanh_project._gradW, "_tanh_project._W", iter);
    checkgrad(examples, _tanh_project._b, _tanh_project._gradb, "_tanh_project._b", iter);

    checkgrad(examples, _rnn_left._rnn_update._WL, _rnn_left._rnn_update._gradWL, "_rnn_left._rnn_update._WL", iter);
    checkgrad(examples, _rnn_left._rnn_update._WR, _rnn_left._rnn_update._gradWR, "_rnn_left._rnn_update._WR", iter);
    checkgrad(examples, _rnn_left._rnn_update._b, _rnn_left._rnn_update._gradb, "_rnn_left._rnn_update._b", iter);
    checkgrad(examples, _rnn_left._rnn_reset._WL, _rnn_left._rnn_reset._gradWL, "_rnn_left._rnn_reset._WL", iter);
    checkgrad(examples, _rnn_left._rnn_reset._WR, _rnn_left._rnn_reset._gradWR, "_rnn_left._rnn_reset._WR", iter);
    checkgrad(examples, _rnn_left._rnn_reset._b, _rnn_left._rnn_reset._gradb, "_rnn_left._rnn_reset._b", iter);
    checkgrad(examples, _rnn_left._rnn._WL, _rnn_left._rnn._gradWL, "_rnn_left._rnn._WL", iter);
    checkgrad(examples, _rnn_left._rnn._WR, _rnn_left._rnn._gradWR, "_rnn_left._rnn._WR", iter);
    checkgrad(examples, _rnn_left._rnn._b, _rnn_left._rnn._gradb, "_rnn_left._rnn._b", iter);

    checkgrad(examples, _rnn_right._rnn_update._WL, _rnn_right._rnn_update._gradWL, "_rnn_right._rnn_update._WL", iter);
    checkgrad(examples, _rnn_right._rnn_update._WR, _rnn_right._rnn_update._gradWR, "_rnn_right._rnn_update._WR", iter);
    checkgrad(examples, _rnn_right._rnn_update._b, _rnn_right._rnn_update._gradb, "_rnn_right._rnn_update._b", iter);
    checkgrad(examples, _rnn_right._rnn_reset._WL, _rnn_right._rnn_reset._gradWL, "_rnn_right._rnn_reset._WL", iter);
    checkgrad(examples, _rnn_right._rnn_reset._WR, _rnn_right._rnn_reset._gradWR, "_rnn_right._rnn_reset._WR", iter);
    checkgrad(examples, _rnn_right._rnn_reset._b, _rnn_right._rnn_reset._gradb, "_rnn_right._rnn_reset._b", iter);
    checkgrad(examples, _rnn_right._rnn._WL, _rnn_right._rnn._gradWL, "_rnn_right._rnn._WL", iter);
    checkgrad(examples, _rnn_right._rnn._WR, _rnn_right._rnn._gradWR, "_rnn_right._rnn._WR", iter);
    checkgrad(examples, _rnn_right._rnn._b, _rnn_right._rnn._gradb, "_rnn_right._rnn._b", iter);

    checkgrad(examples, _recursive._recursive_p._W, _recursive._recursive_p._gradW, "_recursive._recursive_p._W", iter);
    checkgrad(examples, _recursive._recursive_r_other._W, _recursive._recursive_r_other._gradW, "_recursive._recursive_r_other._W", iter);
    for(int i=0;i<_recursive._recursive_r.size();i++) {
    	stringstream ss;
    	ss<<"_recursive._recursive_r["<<i<<"]._W";
    	checkgrad(examples, _recursive._recursive_r[i]._W, _recursive._recursive_r[i]._gradW, ss.str(), iter);
    }
    checkgrad(examples, _recursive._b, _recursive._gradb, "_recursive._b", iter);

    checkgrad(examples, _words._E, _words._gradE, "_words._E", iter, _words._indexers);

  }

public:
  inline void resetEval() {
    _eval.reset();
  }

  inline void setDropValue(dtype dropOut) {
    _dropOut = dropOut;
  }

  inline void setWordEmbFinetune(bool b_wordEmb_finetune) {
    _words.setEmbFineTune(b_wordEmb_finetune);
  }

  inline void resetRemove(int remove) {
    _remove = remove;
  }
};

#endif /* SRC_PoolGRNNClassifier_H_ */
